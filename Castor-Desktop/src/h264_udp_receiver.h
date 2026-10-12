#pragma once

#include <winsock2.h>
#include <d3d11.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "h264_decoder.h"
#include "video_receiver.h" // 复用 CastorVideo::FrameBuffer

namespace CastorVideo {

// 私有 UDP H.264 视频通道接收器：
// 收 UDP 分片 -> 按 frameSeq 重组帧 -> Media Foundation 硬解 -> shared_ptr 环形发布 BGRA。
class H264UdpReceiver {
public:
    H264UdpReceiver();
    ~H264UdpReceiver();

    bool Start(uint16_t port = 27190);
    void Stop();

    // 返回最新解码帧（仅拷贝 shared_ptr，不拷贝像素）
    bool AcquireLatestFrame(FrameBuffer& outFrame, uint64_t lastSeenSeq);

    // 接入 main.cpp 的 D3D11 设备，启用 GPU 转换路径（NV12->BGRA 在 GPU 上做，且显示直接用 GPU 纹理）
    void SetD3DDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx) { d3dDevice_ = dev; d3dCtx_ = ctx; }
    // 当前帧的 GPU BGRA 纹理 SRV（供显示直接用），未启用 GPU 时为 nullptr
    ID3D11ShaderResourceView* GpuSrv() const { return gpuOutSrv_; }

    // GPU 转换（NV12->BGRA）。**必须由持有该 D3D 设备的同一线程（渲染线程）调用**，
    // 因为 D3D11 immediate context 是单线程的。
    bool EnsureGpu(int w, int h);
    bool ConvertOnGpu(const uint8_t* nv12, int w, int h, int strideY);

    float BitrateMbps() const { return mbps_.load(std::memory_order_relaxed); }
    uint64_t TotalFrames() const { return totalFrames_.load(std::memory_order_relaxed); }
    uint64_t DroppedFrames() const { return droppedFrames_.load(std::memory_order_relaxed); }

private:
    void Run();
    void DecodeLoop();
    std::shared_ptr<std::vector<uint8_t>> acquireBgraBuffer(size_t needBytes);

    // GPU 路径：NV12 两平面 -> 纹理 -> pixel shader 转 BGRA -> 输出纹理(SRV 给显示用)
    void ReleaseGpu();

    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dCtx_ = nullptr;
    ID3D11Texture2D* gpuY_ = nullptr;
    ID3D11ShaderResourceView* gpuYSrv_ = nullptr;
    ID3D11Texture2D* gpuUV_ = nullptr;
    ID3D11ShaderResourceView* gpuUVSrv_ = nullptr;
    ID3D11Texture2D* gpuOut_ = nullptr;
    ID3D11ShaderResourceView* gpuOutSrv_ = nullptr;
    ID3D11RenderTargetView* gpuOutRtv_ = nullptr;
    ID3D11VertexShader* gpuVS_ = nullptr;
    ID3D11PixelShader* gpuPS_ = nullptr;
    ID3D11SamplerState* gpuSmp_ = nullptr;
    int gpuW_ = 0, gpuH_ = 0;
    bool gpuFailed_ = false;
    bool gpuMode_ = false;   // 已启用 GPU 路径（解码出 NV12 -> GPU 转 BGRA）

    std::atomic<bool> running_{false};
    std::thread thread_;
    uint16_t port_ = 27190;

    // 收包线程 -> 解码线程 的小队列。
    // WiFi 的 A-MPDU 聚合会把帧"成串"送来，"只留最新"会丢掉一串中间帧；
    // 而丢掉一个 P 帧会打断 H.264 参考链 -> 后续帧带错参考 -> 运动拖影。
    // 故用 FIFO 小队列(深 3) + 缓冲回收池(零逐帧 malloc)：堆积时丢最老并请求关键帧。
    std::mutex frameMtx_;
    std::condition_variable frameCv_;
    std::deque<std::vector<uint8_t>> frameQueue_; // 待解码的完整压缩帧(FIFO)
    std::vector<std::vector<uint8_t>> recycle_;    // 解码完的缓冲回收池
    static constexpr size_t kMaxQueuedFrames = 6;
    std::atomic<bool> needIdr_{false};

    std::mutex mtx_;
    FrameBuffer latest_;

    std::atomic<float> mbps_{0.0f};
    std::atomic<uint64_t> totalFrames_{0};
    std::atomic<uint64_t> droppedFrames_{0};

    static constexpr int kRingSize = 4;
    std::mutex ringMutex_;
    std::shared_ptr<std::vector<uint8_t>> ring_[kRingSize];
    int ringCursor_ = 0;

    H264Decoder decoder_;
};

} // namespace CastorVideo
