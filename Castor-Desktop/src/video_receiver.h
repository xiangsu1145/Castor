#pragma once

#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdint>
#include <memory>
#include <string>

namespace CastorVideo {

struct FrameBuffer {
    // 共享所有权：接收线程写环形缓冲，渲染线程只持有 shared_ptr，
    // 避免渲染侧整帧深拷贝 (1080P≈8MB/帧)。
    std::shared_ptr<std::vector<uint8_t>> rgba;
    int width = 0;
    int height = 0;
    int stride = 0;          // NDI 源行跨度(字节)；缓冲内已按 width*4 紧密排布
    uint64_t seq = 0;
    int64_t captureNs = 0;   // NDI recv 返回时刻 (steady_clock ns)
    int64_t copyDoneNs = 0;  // 拷贝进环形缓冲完成时刻 (steady_clock ns)
    float fps = 0.0f;        // PC 实测 NDI 接收帧率 (所有送达帧计数)

    // 手机端真实耗时 (经 NDI metadata 上报，单位 ms；均为"时长"，不含时钟偏移)
    bool hasPhoneStats = false;
    float phoneFps = 0.0f;
    float phoneIntervalMs = 0.0f;  // 采集帧间隔
    float phoneAcquireMs = 0.0f;   // ImageReader 取帧耗时
    float phoneSendMs = 0.0f;      // RGBA->NDI 异步发送耗时

    std::string clientIp;

    // true = rgba 里装的是原始 NV12（Y 平面 + UV 平面），显存转换由渲染线程在 GPU 上做
    bool nv12Frame = false;
    int nv12Stride = 0;
};

class VideoReceiver {
public:
    VideoReceiver();
    ~VideoReceiver();

    bool Start(uint16_t port = 27183);
    void Stop();

    // 拉取最新解码帧（如果 seq 比 lastSeenSeq 新，返回 true）。仅拷贝 shared_ptr，不拷贝像素。
    bool AcquireLatestFrame(FrameBuffer& outFrame, uint64_t lastSeenSeq);

    // NDI 接收带宽请求: NDIlib_recv_bandwidth_lowest(0) ~ NDIlib_recv_bandwidth_highest(100)。
    // 注意 lowest 会连带让发送端降分辨率。运行时变更在下次建连时生效。
    void SetBandwidth(int bandwidth);

    // NDI 官方诊断 (NDIlib_recv_get_performance / get_queue)：
    // delivered = NDI 实际送达的累计帧数；dropped = 因本端消费不及而丢弃的累计帧数。
    // 若 delivered 低而 dropped≈0 → 帧根本没送达(网络/发送端)；
    // 若 dropped 持续增长 → PC 消费不过来(本端瓶颈)。
    int64_t DeliveredVideoFrames() const { return deliveredVideoFrames_.load(std::memory_order_relaxed); }
    int64_t DroppedVideoFrames() const { return droppedVideoFrames_.load(std::memory_order_relaxed); }
    int QueueVideoDepth() const { return queueVideoDepth_.load(std::memory_order_relaxed); }

private:
    void RunNdiLoop();
    std::shared_ptr<std::vector<uint8_t>> acquireBgraBuffer(size_t needBytes);

    std::atomic<bool> running_{false};
    std::thread workerThread_;

    std::mutex mtx_;
    FrameBuffer latestFrame_;

    // 手机端上报的真实耗时 (NDI metadata, ~1Hz)，由 mtx_ 保护
    bool hasPhoneStats_ = false;
    float phoneFps_ = 0.0f;
    float phoneIntervalMs_ = 0.0f;
    float phoneAcquireMs_ = 0.0f;
    float phoneSendMs_ = 0.0f;

    std::atomic<int> bandwidth_{100};

    std::atomic<int64_t> deliveredVideoFrames_{0};
    std::atomic<int64_t> droppedVideoFrames_{0};
    std::atomic<int> queueVideoDepth_{0};

    static constexpr int kRingSize = 4;
    std::mutex ringMutex_;
    std::shared_ptr<std::vector<uint8_t>> ring_[kRingSize];
    int ringCursor_ = 0;
};

} // namespace CastorVideo
