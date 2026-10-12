#include "video_receiver.h"
#include <Processing.NDI.Lib.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>

namespace CastorVideo {

// 从手机端上报的 NDI metadata XML 里提取 key="value" 形式的浮点属性
static bool extractFloatAttr(const std::string& s, const char* key, float& out) {
    const std::string pat = std::string(key) + "=\"";
    auto p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    auto e = s.find('"', p);
    if (e == std::string::npos || e <= p) return false;
    try {
        out = std::stof(s.substr(p, e - p));
    } catch (...) {
        return false;
    }
    return true;
}

VideoReceiver::VideoReceiver() {}

VideoReceiver::~VideoReceiver() {
    Stop();
}

void VideoReceiver::SetBandwidth(int bandwidth) {
    if (bandwidth < -10) bandwidth = -10;
    if (bandwidth > 100) bandwidth = 100;
    bandwidth_.store(bandwidth, std::memory_order_relaxed);
}

std::shared_ptr<std::vector<uint8_t>> VideoReceiver::acquireBgraBuffer(size_t needBytes) {
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
    // 渲染侧仍持有全部环形缓冲：替换一个槽位，旧数据由渲染侧 shared_ptr 保活
    auto fresh = std::make_shared<std::vector<uint8_t>>(needBytes);
    ring_[ringCursor_] = fresh;
    ringCursor_ = (ringCursor_ + 1) % kRingSize;
    return fresh;
}

bool VideoReceiver::Start(uint16_t /*port*/) {
    if (running_.exchange(true)) return true;
    workerThread_ = std::thread(&VideoReceiver::RunNdiLoop, this);
    return true;
}

void VideoReceiver::Stop() {
    if (running_.exchange(false)) {
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }
}

bool VideoReceiver::AcquireLatestFrame(FrameBuffer& outFrame, uint64_t lastSeenSeq) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (latestFrame_.seq > lastSeenSeq) {
        outFrame = latestFrame_;
        return true;
    }
    return false;
}

void VideoReceiver::RunNdiLoop() {
    // 1. 初始化 WinSock 与 NDI 运行时
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (!NDIlib_initialize()) {
        std::cerr << "[NDI Receiver] 初始化 NDI 运行时失败！" << std::endl;
        WSACleanup();
        return;
    }

    // 2. 创建 UDP 5990 信标监听套接字（零配置穿透局域网防火墙与 AP 组播隔离）
    SOCKET beaconSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (beaconSock != INVALID_SOCKET) {
        BOOL reuse = TRUE;
        setsockopt(beaconSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        DWORD timeoutMs = 100;
        setsockopt(beaconSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(5990);
        bindAddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(beaconSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            closesocket(beaconSock);
            beaconSock = INVALID_SOCKET;
        }
    }

    std::set<std::string> knownIps;
    std::string extraIpsStr;

    auto buildExtraIps = [&knownIps]() -> std::string {
        std::ostringstream oss;
        bool first = true;
        for (const auto& ip : knownIps) {
            if (!first) oss << ",";
            oss << ip;
            first = false;
        }
        return oss.str();
    };

    NDIlib_find_create_t find_desc{};
    find_desc.show_local_sources = true;
    find_desc.p_extra_ips = nullptr;

    // 持久化 Finder 实例：严禁在循环内高频销毁重建，保证 mDNS 与单播探测持续有效
    NDIlib_find_instance_t p_find = NDIlib_find_create_v2(&find_desc);
    if (!p_find) {
        std::cerr << "[NDI Receiver] 创建 NDI Finder 失败！" << std::endl;
        if (beaconSock != INVALID_SOCKET) closesocket(beaconSock);
        NDIlib_destroy();
        WSACleanup();
        return;
    }

    NDIlib_recv_instance_t p_recv = nullptr;
    std::string currentSourceName;

    uint64_t frameSeq = 0;
    auto lastFpsCalc = std::chrono::steady_clock::now();
    auto lastStatsCalc = std::chrono::steady_clock::now();
    uint64_t framesThisSec = 0;
    float currentFps = 60.0f;
    int timeoutCount = 0;

    char beaconBuf[512];

    while (running_.load()) {
        // 3. 接收局域网设备信标广播
        if (beaconSock != INVALID_SOCKET) {
            sockaddr_in fromAddr{};
            int fromLen = sizeof(fromAddr);
            int n = recvfrom(beaconSock, beaconBuf, sizeof(beaconBuf) - 1, 0,
                             reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
            if (n > 0) {
                beaconBuf[n] = '\0';
                std::string msg(beaconBuf);
                // 格式: CASTOR_NDI_BEACON:<streamName>:<ip>
                if (msg.rfind("CASTOR_NDI_BEACON:", 0) == 0) {
                    auto lastColon = msg.rfind(':');
                    if (lastColon != std::string::npos && lastColon + 1 < msg.length()) {
                        std::string ip = msg.substr(lastColon + 1);
                        if (!ip.empty() && knownIps.find(ip) == knownIps.end()) {
                            std::cout << "[NDI Receiver] 接收到移动端信标直连 IP: " << ip << std::endl;
                            knownIps.insert(ip);
                            extraIpsStr = buildExtraIps();

                            // 动态更新单播探测 IP 列表
                            find_desc.p_extra_ips = extraIpsStr.c_str();
                            NDIlib_find_destroy(p_find);
                            p_find = NDIlib_find_create_v2(&find_desc);
                        }
                    }
                }
            }
        }

        // 4. 未连接时：探测并连接信号源
        if (!p_recv) {
            uint32_t num_sources = 0;
            const NDIlib_source_t* p_sources = NDIlib_find_get_current_sources(p_find, &num_sources);

            if (num_sources > 0 && p_sources != nullptr) {
                const auto& targetSource = p_sources[0];
                currentSourceName = targetSource.p_ndi_name ? targetSource.p_ndi_name : "NDI Source";

                NDIlib_recv_create_v3_t recv_desc{};
                recv_desc.source_to_connect_to = targetSource;
                recv_desc.color_format = NDIlib_recv_color_format_BGRX_BGRA; // 直出 BGRA 适配 DX11
                recv_desc.bandwidth = static_cast<NDIlib_recv_bandwidth_e>(bandwidth_.load(std::memory_order_relaxed));
                recv_desc.allow_video_fields = false;
                recv_desc.p_ndi_recv_name = "Castor Desktop Receiver";

                p_recv = NDIlib_recv_create_v3(&recv_desc);
                timeoutCount = 0;
                std::cout << "[NDI Receiver] 瞬时发现并接入信号源: " << currentSourceName << std::endl;
            } else {
                NDIlib_find_wait_for_sources(p_find, 200);
                continue;
            }
        }

        // 4.5 每秒查询 NDI 官方诊断(送达/丢弃/队列)并写盘，便于外部读取真实数据
        if (p_recv) {
            auto statsNow = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(statsNow - lastStatsCalc).count() >= 1000) {
                lastStatsCalc = statsNow;
                NDIlib_recv_performance_t total{}, dropped{};
                NDIlib_recv_get_performance(p_recv, &total, &dropped);
                NDIlib_recv_queue_t q{};
                NDIlib_recv_get_queue(p_recv, &q);
                deliveredVideoFrames_.store(total.video_frames, std::memory_order_relaxed);
                droppedVideoFrames_.store(dropped.video_frames, std::memory_order_relaxed);
                queueVideoDepth_.store(q.video_frames, std::memory_order_relaxed);

                int w = 0, h = 0;
                float pf = 0.0f, pitv = 0.0f, psnd = 0.0f;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    w = latestFrame_.width;
                    h = latestFrame_.height;
                    pf = phoneFps_;
                    pitv = phoneIntervalMs_;
                    psnd = phoneSendMs_;
                }
                FILE* f = std::fopen("castor_ndi_stats.txt", "wb");
                if (f) {
                    char line[320];
                    std::snprintf(line, sizeof(line),
                        "recv_fps=%.1f delivered=%lld dropped=%lld queue=%d w=%d h=%d "
                        "phone_fps=%.1f phone_interval_ms=%.2f phone_send_ms=%.2f\n",
                        currentFps,
                        static_cast<long long>(total.video_frames),
                        static_cast<long long>(dropped.video_frames),
                        q.video_frames, w, h, pf, pitv, psnd);
                    std::fwrite(line, 1, std::strlen(line), f);
                    std::fclose(f);
                }
            }
        }

        // 5. 抓取最新视频帧 (超低超时 20ms)，同时接收手机端上报的 metadata
        NDIlib_video_frame_v2_t video_frame{};
        NDIlib_metadata_frame_t meta_frame{};
        NDIlib_frame_type_e frame_type = NDIlib_recv_capture_v3(
            p_recv, &video_frame, nullptr, &meta_frame, 20);
        auto t1 = std::chrono::steady_clock::now();

        if (frame_type == NDIlib_frame_type_metadata) {
            // 手机端真实分阶段耗时 (~1Hz)
            if (meta_frame.p_data && meta_frame.length > 0) {
                const std::string xml(meta_frame.p_data);
                float v = 0.0f;
                std::lock_guard<std::mutex> lock(mtx_);
                if (extractFloatAttr(xml, "fps", v)) phoneFps_ = v;
                if (extractFloatAttr(xml, "interval_ms", v)) phoneIntervalMs_ = v;
                if (extractFloatAttr(xml, "acquire_ms", v)) phoneAcquireMs_ = v;
                if (extractFloatAttr(xml, "send_ms", v)) phoneSendMs_ = v;
                hasPhoneStats_ = true;
            }
            NDIlib_recv_free_metadata(p_recv, &meta_frame);

        } else if (frame_type == NDIlib_frame_type_video) {
            timeoutCount = 0;

            // 真实接收帧率：每一帧都计数(含下面被排空丢弃的帧)，反映 NDI 实际送达速率
            framesThisSec++;

            // ★ 核心零延迟：排空突发网络缓冲区，始终渲染最前沿最新鲜的一帧
            while (true) {
                NDIlib_video_frame_v2_t next_frame{};
                if (NDIlib_recv_capture_v3(p_recv, &next_frame, nullptr, nullptr, 0) == NDIlib_frame_type_video) {
                    framesThisSec++;
                    NDIlib_recv_free_video_v2(p_recv, &video_frame);
                    video_frame = next_frame;
                } else {
                    break;
                }
            }

            auto nowTime = std::chrono::steady_clock::now();
            auto elapsedFps = std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - lastFpsCalc).count();
            if (elapsedFps >= 1000) {
                currentFps = (framesThisSec * 1000.0f) / static_cast<float>(elapsedFps);
                framesThisSec = 0;
                lastFpsCalc = nowTime;
            }

            int w = video_frame.xres;
            int h = video_frame.yres;
            int stride = video_frame.line_stride_in_bytes > 0 ? video_frame.line_stride_in_bytes : (w * 4);

            if (w > 0 && h > 0 && video_frame.p_data != nullptr) {
                const size_t totalBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
                auto buf = acquireBgraBuffer(totalBytes);
                if (buf) {
                    uint8_t* dst = buf->data();
                    const int copyRowBytes = w * 4;
                    if (stride == copyRowBytes) {
                        std::memcpy(dst, video_frame.p_data, totalBytes);
                    } else {
                        const uint8_t* srcRow = video_frame.p_data;
                        for (int y = 0; y < h; ++y) {
                            std::memcpy(dst + static_cast<size_t>(y) * copyRowBytes, srcRow, copyRowBytes);
                            srcRow += stride;
                        }
                    }
                    auto copyDone = std::chrono::steady_clock::now();

                    std::lock_guard<std::mutex> lock(mtx_);
                    latestFrame_.rgba = buf;
                    latestFrame_.width = w;
                    latestFrame_.height = h;
                    latestFrame_.stride = stride;
                    latestFrame_.seq = ++frameSeq;
                    latestFrame_.captureNs = t1.time_since_epoch().count();
                    latestFrame_.copyDoneNs = copyDone.time_since_epoch().count();
                    latestFrame_.fps = currentFps;
                    latestFrame_.clientIp = currentSourceName;
                    // 附加手机端最近一次上报的真实耗时
                    latestFrame_.hasPhoneStats = hasPhoneStats_;
                    latestFrame_.phoneFps = phoneFps_;
                    latestFrame_.phoneIntervalMs = phoneIntervalMs_;
                    latestFrame_.phoneAcquireMs = phoneAcquireMs_;
                    latestFrame_.phoneSendMs = phoneSendMs_;
                }
            }

            NDIlib_recv_free_video_v2(p_recv, &video_frame);

        } else if (frame_type == NDIlib_frame_type_status_change || frame_type == NDIlib_frame_type_source_change) {
            // 源发生变更
        } else {
            // 超时计数：仅在连续 10 秒无帧（10000ms / 20ms = 500 次）时才重置连接，杜绝网络轻微抖动导致的重连黑屏
            timeoutCount++;
            if (timeoutCount > 500) {
                std::cout << "[NDI Receiver] 信号源已离线，重置接收器" << std::endl;
                NDIlib_recv_destroy(p_recv);
                p_recv = nullptr;
                timeoutCount = 0;
            }
        }
    }

    if (p_recv) {
        NDIlib_recv_destroy(p_recv);
        p_recv = nullptr;
    }
    if (p_find) {
        NDIlib_find_destroy(p_find);
        p_find = nullptr;
    }
    if (beaconSock != INVALID_SOCKET) {
        closesocket(beaconSock);
    }
    NDIlib_destroy();
    WSACleanup();
}

} // namespace CastorVideo
