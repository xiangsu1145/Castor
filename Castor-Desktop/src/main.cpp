#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <tchar.h>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

#include "theme.h"
#include "streaming_ui.h"
#include "network_server.h"
#include "video_receiver.h"
#include "mirror_receiver.h"
#include "mirror_types.h"
#include "h264_udp_receiver.h"
#include "perf_logger.h"

#include <cstdio>
#include <chrono>

// 全局 D3D11 变量
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;

// 动态视频纹理
static ID3D11Texture2D*          g_pVideoTexture = nullptr;
static ID3D11ShaderResourceView* g_pVideoSrv = nullptr;
static int                      currentVideoW = 0;
static int                      currentVideoH = 0;
static uint64_t                 lastSplitSeq = 0;
static uint64_t                 lastUdpSeq = 0;
static uint64_t                 lastH264Seq = 0;

// 当前用于显示的纹理。必须跨帧保持：没有新帧时继续显示上一帧，
// 否则每帧重置会导致画面一闪一闪、大多数帧显示"无画面"。
static ID3D11ShaderResourceView* g_displaySrv = nullptr;
static int                      g_displayW = 0;
static int                      g_displayH = 0;

// 中心原画 (Picture-in-Picture) 纹理
static ID3D11ShaderResourceView* g_roiSrv = nullptr;
static int                      g_roiW = 0;
static int                      g_roiH = 0;
static uint64_t                 lastRoiSeq = 0;

// 前向声明
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 主入口
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)pCmdLine; (void)nCmdShow;

    // 启用高 DPI 感知
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 注册窗口类
    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L, 0L,
        GetModuleHandle(nullptr),
        nullptr, nullptr, nullptr, nullptr,
        L"CastorDesktopWindowClass",
        nullptr
    };
    RegisterClassExW(&wc);

    // 创建窗口 (默认 1280x768)
    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"Castor 极速串流与 AI 推理控制中心 (Direct3D 11 + ImGui)",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1280, 768,
        nullptr, nullptr,
        wc.hInstance,
        nullptr
    );

    // 初始化 Direct3D 11
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // 显示并更新窗口
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // 启动网络后台服务 (UDP 广播发现 + TCP 极速切片串流接收引擎)
    CastorNet::NetworkServer networkServer;
    networkServer.StartBeacon(27183); // 广播无线极速端口 27183

    // 灰狼极速多核切片并发接收解码引擎 (纯无线 Wi-Fi 模式, 默认端口 27183)
    SharedState sharedState;
    std::unique_ptr<Receiver> mirrorReceiver = std::make_unique<Receiver>(sharedState, "", PORT);
    mirrorReceiver->start();

    CastorVideo::VideoReceiver videoReceiver;
    videoReceiver.Start(27183);

    // 私有 H.264 UDP 通道 (手机硬编 H.264 -> UDP 27190 -> PC 硬解)
    CastorVideo::H264UdpReceiver h264Udp;
    h264Udp.SetD3DDevice(g_pd3dDevice, g_pd3dDeviceContext); // 仅存指针；GPU 转换在渲染线程调用
    h264Udp.Start(27190);

    // 私有 H.264 UDP 中心 1:1 原画通道 (手机中心裁剪 -> UDP 27192 -> PC 独立硬解)
    CastorVideo::H264UdpReceiver h264Roi;
    h264Roi.SetD3DDevice(g_pd3dDevice, g_pd3dDeviceContext);
    h264Roi.Start(27192);

    // 初始化 Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // 键盘导航

    // 加载中文字体与美化主题
    CastorUI::LoadChineseFont(io, 18.0f);
    CastorUI::ApplyModernDarkTheme();

    // 绑定 D3D11 & Win32 后端
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 投屏业务与状态实例
    CastorUI::StreamConfig streamConfig;
    std::string localIp = networkServer.GetLocalIpAddress();
    strncpy_s(streamConfig.hostIp, sizeof(streamConfig.hostIp), localIp.c_str(), _TRUNCATE);
    streamConfig.listenPort = 27183;
    streamConfig.beaconPort = 19889;

    CastorUI::StreamingUI streamingUI;

    // 清屏颜色 (深度深空灰背景)
    const float clearColor[4] = { 0.07f, 0.09f, 0.12f, 1.00f };

    // 主消息循环
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) break;

        // 处理用户主动重连或连接指定手机 IP
        if (streamConfig.requestReconnect) {
            streamConfig.requestReconnect = false;
            mirrorReceiver->stop();
            mirrorReceiver = std::make_unique<Receiver>(sharedState, streamConfig.connectTargetIp, streamConfig.connectTargetPort);
            mirrorReceiver->start();
        }

        // 应用 UI 侧的 NDI 接收带宽请求 (重连接收器使其立即生效)
        if (streamConfig.requestApplyBandwidth) {
            streamConfig.requestApplyBandwidth = false;
            videoReceiver.SetBandwidth(streamConfig.bandwidth);
            videoReceiver.Stop();
            videoReceiver.Start(27183);
        }

        // 性能日志录制开关 (导出 huilang_perf_*.csv)
        if (streamConfig.requestTogglePerfLog) {
            streamConfig.requestTogglePerfLog = false;
            bool started = false, saved = false;
            g_perfLog.toggle(started, saved);
            streamConfig.perfRecording = g_perfLog.isRecording();
        }

        // 窗口尺寸发生变化时重建 RenderTarget
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // ImGui 新帧
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 本帧延迟链打点暂存 (NDI 路径)
        int64_t logNdiCaptureNs = 0;
        uint64_t logNdiSeq = 0;
        int logNdiW = 0, logNdiH = 0;
        bool logNdiFrame = false;

        // 1. 检查灰狼极速多核切片并发解码帧 (纯无线 15-20ms 模式)
        DecodedFrame splitFrame;
        bool hasSplitFrame = false;
        {
            std::lock_guard<std::mutex> lk(sharedState.mutex);
            if (!sharedState.status.empty()) {
                WideCharToMultiByte(CP_UTF8, 0, sharedState.status.c_str(), -1,
                                    streamConfig.statusText, sizeof(streamConfig.statusText), nullptr, nullptr);
            }
            if (!sharedState.connectedClientIp.empty()) {
                strncpy_s(streamConfig.clientIp, sizeof(streamConfig.clientIp), sharedState.connectedClientIp.c_str(), _TRUNCATE);
                streamConfig.isConnected = true;
            }
            if (sharedState.hasFrame && sharedState.latest.generation > lastSplitSeq) {
                lastSplitSeq = sharedState.latest.generation;
                splitFrame = sharedState.latest;
                hasSplitFrame = true;
                streamConfig.isConnected = true;
                streamConfig.streamWidth = splitFrame.width;
                streamConfig.streamHeight = splitFrame.height;
                streamConfig.totalFramesReceived++;
                streamConfig.splitParts = sharedState.recvParts;
                streamConfig.currentFps = static_cast<float>(sharedState.recvFps > 0.1 ? sharedState.recvFps : sharedState.decodeFps);
                streamConfig.captureMs = static_cast<float>(splitFrame.captureMs);
                streamConfig.encodeMs = static_cast<float>(splitFrame.encodeMs);
                streamConfig.networkLatencyMs = static_cast<float>(splitFrame.socketMs);
                streamConfig.decodeMs = static_cast<float>(splitFrame.decodeMs);
                streamConfig.totalLatencyMs = static_cast<float>(
                    splitFrame.captureMs + splitFrame.encodeMs + splitFrame.socketMs + splitFrame.decodeMs + streamConfig.renderMs
                );
            }
        }

        // 2. 取帧：私有 H.264 UDP 通道优先，其次 NDI
        CastorVideo::FrameBuffer newFrame;
        bool hasUdpFrame = false;
        if (h264Udp.AcquireLatestFrame(newFrame, lastH264Seq)) {
            lastH264Seq = newFrame.seq;
            hasUdpFrame = true;
            streamConfig.isH264Udp = true;
        } else if (videoReceiver.AcquireLatestFrame(newFrame, lastUdpSeq)) {
            lastUdpSeq = newFrame.seq;
            hasUdpFrame = true;
            streamConfig.isH264Udp = false;
        }
        if (hasUdpFrame) {
            streamConfig.isConnected = true;
            streamConfig.streamWidth = newFrame.width;
            streamConfig.streamHeight = newFrame.height;
            streamConfig.totalFramesReceived++;
            if (!newFrame.clientIp.empty()) {
                strncpy_s(streamConfig.clientIp, sizeof(streamConfig.clientIp), newFrame.clientIp.c_str(), _TRUNCATE);
            }
            if (newFrame.fps > 0.1f) streamConfig.currentFps = newFrame.fps;

            // PC 端实测：抓帧返回 -> 拷贝进缓冲
            if (newFrame.copyDoneNs > newFrame.captureNs) {
                streamConfig.pcRecvMs = static_cast<float>(
                    double(newFrame.copyDoneNs - newFrame.captureNs) / 1000000.0);
            }
            // 手机端实测 (仅 NDI 通道经 metadata 上报)
            streamConfig.hasPhoneStats = newFrame.hasPhoneStats;
            streamConfig.phoneFps = newFrame.phoneFps;
            streamConfig.phoneIntervalMs = newFrame.phoneIntervalMs;
            streamConfig.phoneAcquireMs = newFrame.phoneAcquireMs;
            streamConfig.phoneSendMs = newFrame.phoneSendMs;
        }
        streamConfig.h264UdpMbps = h264Udp.BitrateMbps();

        if (hasUdpFrame && newFrame.nv12Frame && newFrame.rgba && !newFrame.rgba->empty()) {
            // 私有 UDP 通道：收原始 NV12，在渲染线程用 GPU 转 BGRA，免 CPU 转换 + 免 CPU 上传
            if (h264Udp.EnsureGpu(newFrame.width, newFrame.height) &&
                h264Udp.ConvertOnGpu(newFrame.rgba->data(), newFrame.width, newFrame.height, newFrame.nv12Stride)) {
                g_displaySrv = h264Udp.GpuSrv();
                g_displayW = newFrame.width;
                g_displayH = newFrame.height;
            }
            logNdiSeq = newFrame.seq;
            logNdiW = newFrame.width;
            logNdiH = newFrame.height;
            logNdiFrame = true;
        } else if (hasUdpFrame && newFrame.rgba && !newFrame.rgba->empty()) {
            // NDI 帧更新
            if (!g_pVideoTexture || currentVideoW != newFrame.width || currentVideoH != newFrame.height) {
                if (g_pVideoSrv) { g_pVideoSrv->Release(); g_pVideoSrv = nullptr; }
                if (g_pVideoTexture) { g_pVideoTexture->Release(); g_pVideoTexture = nullptr; }

                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = newFrame.width;
                desc.Height = newFrame.height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_pVideoTexture))) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format = desc.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    g_pd3dDevice->CreateShaderResourceView(g_pVideoTexture, &srvDesc, &g_pVideoSrv);
                    currentVideoW = newFrame.width;
                    currentVideoH = newFrame.height;
                }
            }

            if (g_pVideoTexture) {
                auto renderT0 = std::chrono::steady_clock::now();
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(g_pd3dDeviceContext->Map(g_pVideoTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    const int rowBytes = newFrame.width * 4;
                    const uint8_t* src = newFrame.rgba->data();
                    if (static_cast<int>(mapped.RowPitch) == rowBytes) {
                        memcpy(mapped.pData, src, static_cast<size_t>(rowBytes) * newFrame.height);
                    } else {
                        for (int y = 0; y < newFrame.height; ++y) {
                            memcpy(reinterpret_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                                   src + static_cast<size_t>(y) * rowBytes, rowBytes);
                        }
                    }
                    g_pd3dDeviceContext->Unmap(g_pVideoTexture, 0);
                }
                auto renderT1 = std::chrono::steady_clock::now();
                streamConfig.renderMs = static_cast<float>(
                    std::chrono::duration_cast<std::chrono::microseconds>(renderT1 - renderT0).count()) / 1000.0f;
                logNdiCaptureNs = newFrame.captureNs;
                logNdiSeq = newFrame.seq;
                logNdiW = newFrame.width;
                logNdiH = newFrame.height;
                logNdiFrame = true;
                g_displaySrv = g_pVideoSrv;
                g_displayW = currentVideoW;
                g_displayH = currentVideoH;
            }
        } else if (hasSplitFrame && splitFrame.pixelsBGRA && !splitFrame.pixelsBGRA->empty()) {
            // 切片流帧更新
            if (!g_pVideoTexture || currentVideoW != splitFrame.width || currentVideoH != splitFrame.height) {
                if (g_pVideoSrv) { g_pVideoSrv->Release(); g_pVideoSrv = nullptr; }
                if (g_pVideoTexture) { g_pVideoTexture->Release(); g_pVideoTexture = nullptr; }

                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = splitFrame.width;
                desc.Height = splitFrame.height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_pVideoTexture))) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format = desc.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    g_pd3dDevice->CreateShaderResourceView(g_pVideoTexture, &srvDesc, &g_pVideoSrv);
                    currentVideoW = splitFrame.width;
                    currentVideoH = splitFrame.height;
                }
            }

            if (g_pVideoTexture) {
                auto renderT0 = std::chrono::steady_clock::now();
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(g_pd3dDeviceContext->Map(g_pVideoTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    const int rowBytes = splitFrame.width * 4;
                    const uint8_t* src = splitFrame.pixelsBGRA->data();
                    if (static_cast<int>(mapped.RowPitch) == rowBytes) {
                        memcpy(mapped.pData, src, static_cast<size_t>(rowBytes) * splitFrame.height);
                    } else {
                        for (int y = 0; y < splitFrame.height; ++y) {
                            memcpy(reinterpret_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                                   src + static_cast<size_t>(y) * rowBytes, rowBytes);
                        }
                    }
                    g_pd3dDeviceContext->Unmap(g_pVideoTexture, 0);
                }
                auto renderT1 = std::chrono::steady_clock::now();
                streamConfig.renderMs = static_cast<float>(
                    std::chrono::duration_cast<std::chrono::microseconds>(renderT1 - renderT0).count()) / 1000.0f;
                g_displaySrv = g_pVideoSrv;
                g_displayW = currentVideoW;
                g_displayH = currentVideoH;
            }
        }

        // NDI 官方诊断 (送达/丢弃/队列) —— 判断是"帧没送达"还是"PC 消费不过来"
        streamConfig.ndiDelivered = videoReceiver.DeliveredVideoFrames();
        streamConfig.ndiDropped = videoReceiver.DroppedVideoFrames();
        streamConfig.ndiQueue = videoReceiver.QueueVideoDepth();

        // 2.1 尝试获取中心 1:1 原画帧 (UDP 27192)
        CastorVideo::FrameBuffer newRoiFrame;
        bool hasRoiFrame = false;
        if (h264Roi.AcquireLatestFrame(newRoiFrame, lastRoiSeq)) {
            lastRoiSeq = newRoiFrame.seq;
            hasRoiFrame = true;
        }
        if (hasRoiFrame && newRoiFrame.nv12Frame && newRoiFrame.rgba && !newRoiFrame.rgba->empty()) {
            if (h264Roi.EnsureGpu(newRoiFrame.width, newRoiFrame.height) &&
                h264Roi.ConvertOnGpu(newRoiFrame.rgba->data(), newRoiFrame.width, newRoiFrame.height, newRoiFrame.nv12Stride)) {
                g_roiSrv = h264Roi.GpuSrv();
                g_roiW = newRoiFrame.width;
                g_roiH = newRoiFrame.height;
            }
        }
        streamConfig.hasRoiStream = (g_roiSrv != nullptr && g_roiW > 0);
        streamConfig.roiWidth = g_roiW;
        streamConfig.roiHeight = g_roiH;
        if (hasRoiFrame && newRoiFrame.fps > 0.1f) streamConfig.roiFps = newRoiFrame.fps;
        streamConfig.roiMbps = h264Roi.BitrateMbps();

        streamConfig.renderFps = ImGui::GetIO().Framerate;

        // 渲染主界面 (传入真实视频背景纹理 + 中心 1:1 原画纹理)
        RECT rect;
        GetClientRect(hwnd, &rect);
        float width = static_cast<float>(rect.right - rect.left);
        float height = static_cast<float>(rect.bottom - rect.top);
        streamingUI.Render(streamConfig, width, height, g_displaySrv, g_displayW, g_displayH, g_roiSrv, g_roiW, g_roiH);

        // 渲染输出
        auto drawT0 = std::chrono::steady_clock::now();
        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        auto drawT1 = std::chrono::steady_clock::now();

        // 呈现画面 (传 0 关闭垂直同步等待，达到极致 0 延迟即时翻转显存)
        g_pSwapChain->Present(0, 0);
        auto presentT1 = std::chrono::steady_clock::now();

        // PC 端真实分阶段耗时 (全部实测，无编造)
        const double drawMs = std::chrono::duration_cast<std::chrono::microseconds>(drawT1 - drawT0).count() / 1000.0;
        const double presentMs = std::chrono::duration_cast<std::chrono::microseconds>(presentT1 - drawT1).count() / 1000.0;
        streamConfig.pcUploadMs = streamConfig.renderMs;
        streamConfig.pcDrawMs = static_cast<float>(drawMs);
        streamConfig.pcPresentMs = static_cast<float>(presentMs);
        streamConfig.measuredSumMs = streamConfig.phoneSendMs + streamConfig.pcRecvMs
            + streamConfig.pcUploadMs + streamConfig.pcDrawMs + streamConfig.pcPresentMs;

        // 延迟链打点: 手机端(取帧/发送) + PC(抓帧/上传/绘制/呈现) 全为实测值
        if (logNdiFrame && g_perfLog.isRecording()) {
            const double capToPresentMs = logNdiCaptureNs > 0
                ? double(presentT1.time_since_epoch().count() - logNdiCaptureNs) / 1000000.0
                : 0.0;
            char payload[384];
            std::snprintf(payload, sizeof(payload),
                "seq=%llu,w=%d,h=%d,fps=%.1f,phone_fps=%.1f,phone_interval_ms=%.2f,phone_acquire_ms=%.2f,phone_send_ms=%.2f,"
                "pc_recv_ms=%.2f,upload_ms=%.2f,draw_ms=%.2f,present_ms=%.2f,cap_to_present_ms=%.2f",
                static_cast<unsigned long long>(logNdiSeq), logNdiW, logNdiH,
                streamConfig.currentFps, streamConfig.phoneFps, streamConfig.phoneIntervalMs,
                streamConfig.phoneAcquireMs, streamConfig.phoneSendMs, streamConfig.pcRecvMs,
                streamConfig.pcUploadMs, drawMs, presentMs, capToPresentMs);
            g_perfLog.recordRow("ndi_frame", payload);
        }
    }

    // 清理资源
    if (g_pVideoSrv) { g_pVideoSrv->Release(); g_pVideoSrv = nullptr; }
    if (g_pVideoTexture) { g_pVideoTexture->Release(); g_pVideoTexture = nullptr; }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2; // 双缓冲
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 0; // Flip 模式自由呈现
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // 现代 Flip 模式，绕过 DWM 中间缓冲与垂直同步延迟

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    // createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray, 2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext
    );

    // 若驱动不支持 FLIP_DISCARD，自动回退到经典 DISCARD
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferDesc.RefreshRate.Numerator = 144;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevelArray, 2,
            D3D11_SDK_VERSION,
            &sd,
            &g_pSwapChain,
            &g_pd3dDevice,
            &featureLevel,
            &g_pd3dDeviceContext
        );
    }

    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

// 转发 ImGui 消息
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // 禁用 Alt 菜单激活
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
