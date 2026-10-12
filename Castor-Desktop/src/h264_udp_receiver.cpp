#include "h264_udp_receiver.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace CastorVideo {

// 追加一行诊断日志到 CWD 下的 castor_h264_udp.log（GUI 程序无控制台，只能写文件）
static void ULog(const char* fmt, ...) {
    FILE* f = std::fopen("castor_h264_udp.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

static constexpr uint32_t MAGIC_H264 = 0x34363248u; // 'H''2''6''4' 小端
static constexpr int HDR_SIZE = 24;
static constexpr int FRAG_PAYLOAD = 1400;

#pragma pack(push, 1)
struct H264Hdr {
    uint32_t magic;
    uint32_t frameSeq;
    uint16_t fragIndex;
    uint16_t fragCount;
    uint16_t len;
    uint16_t flags;
    uint64_t ptsUs;
};
#pragma pack(pop)

H264UdpReceiver::H264UdpReceiver() {}
H264UdpReceiver::~H264UdpReceiver() { Stop(); }

bool H264UdpReceiver::Start(uint16_t port) {
    if (running_.exchange(true)) return true;
    port_ = port;
    thread_ = std::thread(&H264UdpReceiver::Run, this);
    return true;
}

void H264UdpReceiver::Stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        decoder_.Shutdown();
    }
}

bool H264UdpReceiver::AcquireLatestFrame(FrameBuffer& outFrame, uint64_t lastSeenSeq) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (latest_.seq > lastSeenSeq) {
        outFrame = latest_;
        return true;
    }
    return false;
}

std::shared_ptr<std::vector<uint8_t>> H264UdpReceiver::acquireBgraBuffer(size_t needBytes) {
    if (needBytes == 0) return nullptr;
    std::lock_guard<std::mutex> lk(ringMutex_);
    for (int attempt = 0; attempt < kRingSize; ++attempt) {
        const int idx = (ringCursor_ + attempt) % kRingSize;
        auto& buf = ring_[idx];
        if (!buf || buf.use_count() == 1) {
            if (!buf) buf = std::make_shared<std::vector<uint8_t>>();
            if (buf->capacity() < needBytes) buf->reserve(needBytes);
            buf->resize(needBytes);
            ringCursor_ = (idx + 1) % kRingSize;
            return buf;
        }
    }
    auto fresh = std::make_shared<std::vector<uint8_t>>(needBytes);
    ring_[ringCursor_] = fresh;
    ringCursor_ = (ringCursor_ + 1) % kRingSize;
    return fresh;
}

void H264UdpReceiver::Run() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        std::cerr << "[H264UDP] socket 创建失败" << std::endl;
        WSACleanup();
        return;
    }
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    DWORD timeoutMs = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ULog("[H264UDP] bind FAILED port=%u err=%d", port_, WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return;
    }
    ULog("[H264UDP] bind OK, listening UDP %u", port_);

    decoder_.Init();

    // 解码线程：与收包线程解耦，只取"最新"一帧解码，旧的直接丢（反缓冲膨胀）
    std::thread decoderThread([this]() { DecodeLoop(); });

    struct Pending {
        std::vector<std::vector<uint8_t>> frags;
        std::vector<bool> have;
        int fragCount = 0;
        int received = 0;
    };
    std::unordered_map<uint32_t, Pending> pending;

    std::vector<uint8_t> recvBuf(HDR_SIZE + FRAG_PAYLOAD + 64);

    uint64_t bytesThisSec = 0;
    uint64_t pktsThisSec = 0;
    uint64_t completedThisSec = 0;
    int loggedCount = 0;
    sockaddr_in lastFrom{};
    int lastFromLen = 0;
    auto lastIdrReq = std::chrono::steady_clock::now();
    auto winStart = std::chrono::steady_clock::now();

    while (running_.load()) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(s, reinterpret_cast<char*>(recvBuf.data()),
                         static_cast<int>(recvBuf.size()), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n > 0) { lastFrom = from; lastFromLen = fromLen; }
        if (n >= HDR_SIZE) {
            H264Hdr h;
            std::memcpy(&h, recvBuf.data(), sizeof(H264Hdr));
            if (h.magic == MAGIC_H264 && h.fragCount > 0 && h.fragIndex < h.fragCount && h.len <= FRAG_PAYLOAD
                && n >= HDR_SIZE + h.len) {
                bytesThisSec += n;
                pktsThisSec++;
                auto& p = pending[h.frameSeq];
                if (p.fragCount == 0) {
                    p.fragCount = h.fragCount;
                    p.frags.assign(h.fragCount, {});
                    p.have.assign(h.fragCount, false);
                    if (pending.size() > 8) {
                        uint32_t oldestKey = 0; bool found = false;
                        for (auto& kv : pending) { if (!found || kv.first < oldestKey) { oldestKey = kv.first; found = true; } }
                        if (found && oldestKey != h.frameSeq) { pending.erase(oldestKey); droppedFrames_.fetch_add(1); }
                    }
                }
                if (!p.have[h.fragIndex]) {
                    p.frags[h.fragIndex].assign(recvBuf.begin() + HDR_SIZE, recvBuf.begin() + HDR_SIZE + h.len);
                    p.have[h.fragIndex] = true;
                    p.received++;
                }
                if (p.received == p.fragCount) {
                    const int fragCount = p.fragCount;

                    // 从回收池取一块缓冲复用，避免逐帧 malloc
                    std::vector<uint8_t> frame;
                    {
                        std::lock_guard<std::mutex> lk(frameMtx_);
                        if (!recycle_.empty()) { frame = std::move(recycle_.back()); recycle_.pop_back(); }
                    }
                    frame.clear();
                    for (auto& fr : p.frags) frame.insert(frame.end(), fr.begin(), fr.end());
                    pending.erase(h.frameSeq);
                    completedThisSec++;

                    if (loggedCount < 4 && !frame.empty()) {
                        char hex[64] = {0};
                        const int hn = frame.size() < 12 ? static_cast<int>(frame.size()) : 12;
                        for (int i = 0; i < hn; ++i) std::snprintf(hex + i * 3, 4, "%02X ", frame[i]);
                        ULog("frame seq=%u size=%zu frags=%d head=[%s]", h.frameSeq, frame.size(), fragCount, hex);
                        loggedCount++;
                    }

                    if (!frame.empty()) {
                        // 入 FIFO 小队列。满则丢最老：丢帧会打断参考链，故同时请求关键帧。
                        std::lock_guard<std::mutex> lk(frameMtx_);
                        if (frameQueue_.size() >= kMaxQueuedFrames) {
                            recycle_.push_back(std::move(frameQueue_.front()));
                            frameQueue_.pop_front();
                            droppedFrames_.fetch_add(1);
                            needIdr_.store(true);
                        }
                        frameQueue_.push_back(std::move(frame));
                        frameCv_.notify_one();
                    }
                }
            }
        }

        // 解码线程解不出画面 / 队列丢帧时会置 needIdr_，这里限速回发 !IDR 让手机补关键帧。
        // 注意：限速窗口内不清除标志，等窗口一开就补发（否则丢帧触发的请求会被白白吞掉）。
        if (needIdr_.load() && lastFromLen > 0) {
            auto nowIdr = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowIdr - lastIdrReq).count() >= 500) {
                lastIdrReq = nowIdr;
                needIdr_.store(false);
                const char idrReq[4] = {'!', 'I', 'D', 'R'};
                sendto(s, idrReq, 4, 0, reinterpret_cast<sockaddr*>(&lastFrom), lastFromLen);
            }
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - winStart).count();
        if (elapsedMs >= 1000) {
            mbps_.store(static_cast<float>(bytesThisSec * 8.0 / (elapsedMs / 1000.0) / 1e6));
            ULog("recv: pkts=%llu completed=%llu | %.1f Mbps | decodedTotal=%llu dropped=%llu | pending=%zu",
                 static_cast<unsigned long long>(pktsThisSec),
                 static_cast<unsigned long long>(completedThisSec),
                 mbps_.load(),
                 static_cast<unsigned long long>(totalFrames_.load()),
                 static_cast<unsigned long long>(droppedFrames_.load()),
                 pending.size());
            bytesThisSec = 0;
            pktsThisSec = 0;
            completedThisSec = 0;
            winStart = now;
        }
    }

    frameCv_.notify_all();
    if (decoderThread.joinable()) decoderThread.join();
    closesocket(s);
    WSACleanup();
}

void H264UdpReceiver::DecodeLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::vector<uint8_t> local;
    std::vector<uint8_t> bgra;
    int dw = 0, dh = 0;
    int framesThisSec = 0;
    int failsThisSec = 0;
    auto winStart = std::chrono::steady_clock::now();
    uint64_t outSeq = 0;
    float fps = 0.0f;
    double waitSumMs = 0, decodeSumMs = 0, pubSumMs = 0;
    int timingCount = 0;

    while (running_.load()) {
        const auto tLoop0 = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::mutex> lk(frameMtx_);
            frameCv_.wait(lk, [this] { return !running_.load() || !frameQueue_.empty(); });
            if (!running_.load()) break;
            local.swap(frameQueue_.front());
            recycle_.push_back(std::move(frameQueue_.front())); // 回收上一帧的缓冲
            frameQueue_.pop_front();
        }
        if (local.empty()) continue;

        auto t0 = std::chrono::steady_clock::now();
        int stride = 0;
        // 只解出原始 NV12；NV12->BGRA 的 GPU 转换由渲染线程做（D3D immediate context 单线程）
        const bool ok = decoder_.DecodeFrame(local.data(), local.size(), bgra, dw, dh, true, &stride);
        auto t1 = std::chrono::steady_clock::now();
        const double decodeMs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;

        if (ok && dw > 0 && dh > 0 && !bgra.empty()) {
            auto buf = acquireBgraBuffer(bgra.size());
            if (buf) {
                buf->swap(bgra);
                std::lock_guard<std::mutex> lk(mtx_);
                latest_.rgba = buf;
                latest_.nv12Frame = true;
                latest_.nv12Stride = stride;
                latest_.width = dw;
                latest_.height = dh;
                latest_.stride = dw * 4;
                latest_.seq = ++outSeq;
                latest_.fps = fps;
                latest_.captureNs = std::chrono::steady_clock::now().time_since_epoch().count();
                latest_.clientIp = "UDP-H264";
                totalFrames_.fetch_add(1);
            }
            framesThisSec++;
        } else {
            failsThisSec++;
            needIdr_.store(true);
        }

        const auto t2 = std::chrono::steady_clock::now();
        const double waitMs = std::chrono::duration_cast<std::chrono::microseconds>(t0 - tLoop0).count() / 1000.0;
        const double pubMs = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / 1000.0;
        waitSumMs += waitMs; decodeSumMs += decodeMs; pubSumMs += pubMs; timingCount++;

        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - winStart).count();
        if (elapsedMs >= 1000) {
            fps = framesThisSec * 1000.0f / static_cast<float>(elapsedMs);
            const int n = timingCount > 0 ? timingCount : 1;
            ULog("decode: %.1f fps | 解=%.1fms 等待=%.1fms 发布=%.1fms | fails=%d",
                 fps, decodeSumMs / n, waitSumMs / n, pubSumMs / n, failsThisSec);
            framesThisSec = 0;
            failsThisSec = 0;
            waitSumMs = decodeSumMs = pubSumMs = 0;
            timingCount = 0;
            winStart = now;
        }
    }
}

// ---------------- GPU 路径：NV12 -> BGRA 用 pixel shader 在 GPU 上转换 ----------------
static const char* kConvHlsl = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
Texture2D<float>  yTex  : register(t0);
Texture2D<float2> uvTex : register(t1);
SamplerState      smp   : register(s0);
float4 PSMain(VSOut i) : SV_Target {
    float  y = yTex.Sample(smp, i.uv).r;
    float2 c = uvTex.Sample(smp, i.uv).rg;
    float Y = (y - 0.0627451) * 1.164;   // BT.601 limited range
    float U = c.x - 0.5;
    float V = c.y - 0.5;
    float3 rgb = float3(Y + 1.596*V, Y - 0.391*U - 0.813*V, Y + 2.018*U);
    return float4(rgb, 1.0);
}
)";

void H264UdpReceiver::ReleaseGpu() {
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    rel(gpuSmp_); rel(gpuPS_); rel(gpuVS_);
    rel(gpuOutRtv_); rel(gpuOutSrv_); rel(gpuOut_);
    rel(gpuUVSrv_); rel(gpuUV_);
    rel(gpuYSrv_); rel(gpuY_);
    gpuW_ = gpuH_ = 0;
}

bool H264UdpReceiver::EnsureGpu(int w, int h) {
    if (gpuOut_ && gpuW_ == w && gpuH_ == h) return true;
    if (gpuFailed_) return false;
    ReleaseGpu();
    if (!d3dDevice_ || w <= 0 || h <= 0) { gpuFailed_ = true; return false; }

    D3D11_TEXTURE2D_DESC td{};
    td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    td.Width = w; td.Height = h; td.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(d3dDevice_->CreateTexture2D(&td, nullptr, &gpuY_))) { gpuFailed_ = true; return false; }
    d3dDevice_->CreateShaderResourceView(gpuY_, nullptr, &gpuYSrv_);
    td.Width = w / 2; td.Height = h / 2; td.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(d3dDevice_->CreateTexture2D(&td, nullptr, &gpuUV_))) { gpuFailed_ = true; return false; }
    d3dDevice_->CreateShaderResourceView(gpuUV_, nullptr, &gpuUVSrv_);

    D3D11_TEXTURE2D_DESC od{};
    od.Width = w; od.Height = h; od.MipLevels = 1; od.ArraySize = 1; od.SampleDesc.Count = 1;
    od.Format = DXGI_FORMAT_B8G8R8A8_UNORM; od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3dDevice_->CreateTexture2D(&od, nullptr, &gpuOut_))) { gpuFailed_ = true; return false; }
    d3dDevice_->CreateShaderResourceView(gpuOut_, nullptr, &gpuOutSrv_);
    if (FAILED(d3dDevice_->CreateRenderTargetView(gpuOut_, nullptr, &gpuOutRtv_))) { gpuFailed_ = true; return false; }

    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    HRESULT hv = D3DCompile(kConvHlsl, strlen(kConvHlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsb, &err);
    HRESULT hp = D3DCompile(kConvHlsl, strlen(kConvHlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hv) || FAILED(hp)) {
        if (err) ULog("GPU shader 编译失败: %s", (const char*)err->GetBufferPointer());
        if (err) err->Release();
        if (vsb) vsb->Release();
        if (psb) psb->Release();
        gpuFailed_ = true; return false;
    }
    d3dDevice_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &gpuVS_);
    d3dDevice_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &gpuPS_);
    vsb->Release(); psb->Release();

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    d3dDevice_->CreateSamplerState(&sd, &gpuSmp_);

    gpuW_ = w; gpuH_ = h;
    ULog("GPU 转换就绪: %dx%d NV12->BGRA (shader)", w, h);
    return true;
}

bool H264UdpReceiver::ConvertOnGpu(const uint8_t* nv12, int w, int h, int strideY) {
    if (!d3dCtx_ || !gpuOutRtv_ || !gpuVS_ || !gpuPS_ || !gpuSmp_) return false;
    const uint8_t* yPlane = nv12;
    const uint8_t* uvPlane = nv12 + static_cast<size_t>(strideY) * h;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(d3dCtx_->Map(gpuY_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, yPlane + static_cast<size_t>(y) * strideY, w);
    d3dCtx_->Unmap(gpuY_, 0);

    if (FAILED(d3dCtx_->Map(gpuUV_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    for (int y = 0; y < h / 2; ++y)
        std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, uvPlane + static_cast<size_t>(y) * strideY, w);
    d3dCtx_->Unmap(gpuUV_, 0);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    d3dCtx_->RSSetViewports(1, &vp);
    d3dCtx_->OMSetRenderTargets(1, &gpuOutRtv_, nullptr);
    d3dCtx_->IASetInputLayout(nullptr);
    d3dCtx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3dCtx_->VSSetShader(gpuVS_, nullptr, 0);
    d3dCtx_->PSSetShader(gpuPS_, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { gpuYSrv_, gpuUVSrv_ };
    d3dCtx_->PSSetShaderResources(0, 2, srvs);
    d3dCtx_->PSSetSamplers(0, 1, &gpuSmp_);
    d3dCtx_->Draw(3, 0);
    // 解绑，避免与显示端读同一资源冲突
    ID3D11ShaderResourceView* nullSrv[2] = { nullptr, nullptr };
    d3dCtx_->PSSetShaderResources(0, 2, nullSrv);
    d3dCtx_->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

} // namespace CastorVideo
