#pragma once
#include <string>
#include <vector>

namespace CastorUI {

struct StreamConfig {
    // 本机网络服务信息 (供手机端连接)
    char hostIp[32] = "192.168.31.135";
    int listenPort = 19888;
    int beaconPort = 19889;

    // 手机端接入状态
    bool isConnected = false;
    char clientIp[32] = "";
    char statusText[128] = "等待连接...";
    int streamWidth = 0;
    int streamHeight = 0;
    uint64_t totalFramesReceived = 0;

    // 目标设备连接控制 (灰狼极速分块模式)
    char connectTargetIp[32] = "";
    int connectTargetPort = 27183;
    bool requestReconnect = false;
    int splitParts = 0;
    float captureMs = 0.0f;
    float encodeMs = 0.0f;
    float currentFps = 0.0f;
    float renderFps = 0.0f;
    float decodeMs = 0.0f;
    float renderMs = 0.0f;
    float networkLatencyMs = 0.0f;
    float totalLatencyMs = 0.0f;
    std::vector<float> fpsHistory;

    // ---- NDI 路径真实实测数据 (不再有任何编造值) ----
    bool  hasPhoneStats = false;     // 手机端是否已上报真实耗时
    float phoneFps = 0.0f;           // 手机端实测发送帧率
    float phoneIntervalMs = 0.0f;    // 手机端采集帧间隔
    float phoneAcquireMs = 0.0f;     // 手机端取帧耗时
    float phoneSendMs = 0.0f;        // 手机端 RGBA->NDI 异步发送耗时
    float pcRecvMs = 0.0f;           // PC：NDI 抓帧返回 -> 拷贝进缓冲
    float pcUploadMs = 0.0f;         // PC：上传 D3D11 纹理
    float pcDrawMs = 0.0f;           // PC：ImGui 绘制
    float pcPresentMs = 0.0f;        // PC：Present
    float measuredSumMs = 0.0f;      // 已实测各段合计(故意不叫"端到端")

    // NDI 官方诊断：送达/丢弃/队列。低 delivered + dropped≈0 → 帧没送达(网络/发送端)；
    // dropped 持续增长 → PC 消费不过来。
    int64_t ndiDelivered = 0;
    int64_t ndiDropped = 0;
    int ndiQueue = 0;

    // 画面接收模式：0 = 自动检测, 1 = UDP 模式 (默认/低延迟), 2 = NDI 模式 (兼容)
    int receiveMode = 1;
    bool requestResetStream = false;     // 重置/刷新接收
    bool requestDisconnect = false;      // 断开当前画面

    // 当前通道状态
    bool isH264Udp = true;
    float h264UdpMbps = 0.0f;

    // 中心原画 (Picture-in-Picture / ROI) 状态
    bool hasRoiStream = false;
    int roiWidth = 0;
    int roiHeight = 0;
    float roiFps = 0.0f;
    float roiMbps = 0.0f;
    bool showRoiBoundary = true; // 是否在中心原画四周显示微弱定位框

    // 画面与辅助功能设置
    bool showCrosshair = false;
    bool enableCenterRoiOnly = false;
    int roiPercent = 50;          // 中心区域占比
    bool showHud = true;

    // 后台控制字段
    int bandwidth = 100;
    bool requestApplyBandwidth = false;
    bool requestTogglePerfLog = false;
    bool perfRecording = false;
};

class StreamingUI {
public:
    StreamingUI();
    void Render(StreamConfig& config, float screenWidth, float screenHeight,
                void* videoTextureSrv = nullptr, int videoW = 0, int videoH = 0,
                void* roiTextureSrv = nullptr, int roiW = 0, int roiH = 0);

private:
    void RenderSidebar(StreamConfig& config);
    void RenderMainView(StreamConfig& config, void* videoTextureSrv, int videoW, int videoH,
                        void* roiTextureSrv = nullptr, int roiW = 0, int roiH = 0);
    void RenderHudOverlay(const StreamConfig& config);
    void RenderModelManagementSection();
    void RenderImportModelModal();
};

} // namespace CastorUI
