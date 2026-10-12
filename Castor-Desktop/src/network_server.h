#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>

namespace CastorNet {

struct LiveStreamStats {
    bool isConnected = false;
    std::string clientIp;
    float currentFps = 0.0f;
    float bitrateMbps = 0.0f;
    float latencyMs = 0.0f;
    uint64_t totalFrames = 0;
    uint64_t totalBytes = 0;
};

class NetworkServer {
public:
    NetworkServer();
    ~NetworkServer();

    // 启动 UDP 局域网广播与发现应答
    bool StartBeacon(int listenPort = 19888);
    void StopBeacon();

    // 启动 TCP 极低延迟串流接收服务
    bool StartStreamServer(int listenPort = 19888);
    void StopStreamServer();

    // 获取最新统计数据
    LiveStreamStats GetStats() const;
    std::string GetLocalIpAddress() const;

private:
    void RunBeaconLoop(int listenPort);
    void RunServerLoop(int listenPort);

    std::atomic<bool> beaconRunning_{false};
    std::atomic<bool> serverRunning_{false};

    std::thread beaconThread_;
    std::thread serverThread_;

    mutable LiveStreamStats stats_;
};

} // namespace CastorNet
