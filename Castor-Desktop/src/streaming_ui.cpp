#include "streaming_ui.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace CastorUI {

StreamingUI::StreamingUI() {
}

void StreamingUI::Render(StreamConfig& config, float screenWidth, float screenHeight,
                         void* videoTextureSrv, int videoW, int videoH) {
    if (config.isConnected && config.currentFps > 0.1f) {
        config.fpsHistory.push_back(config.currentFps);
        if (config.fpsHistory.size() > 120) {
            config.fpsHistory.erase(config.fpsHistory.begin());
        }
    }

    // 全屏 Dock 容器窗口
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenWidth, screenHeight));
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("CastorRootWindow", nullptr, windowFlags);
    ImGui::PopStyleVar(2);

    // 左侧面板 (参数与监控) 占 380px 宽，右侧为主画面
    const float sidebarWidth = 380.0f;
    ImGui::BeginChild("SidebarPanel", ImVec2(sidebarWidth, 0), true);
    RenderSidebar(config);
    ImGui::EndChild();

    ImGui::SameLine();

    // 右侧投屏画面区域
    ImGui::BeginChild("ScreenDisplayArea", ImVec2(0, 0), true);
    RenderMainView(config, videoTextureSrv, videoW, videoH);
    if (config.showHud) {
        RenderHudOverlay(config);
    }
    ImGui::EndChild();

    ImGui::End();
}

void StreamingUI::RenderSidebar(StreamConfig& config) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.00f, 0.86f, 0.88f, 1.0f), "CASTOR NDI 极速串流中心");
    ImGui::TextDisabled("协议: NewTek NDI 6 广电级低延迟传输 | 渲染: DirectX 11");
    ImGui::Separator();
    ImGui::Spacing();

    // 卡片 1: NDI 局域网服务状态
    if (ImGui::CollapsingHeader("NDI 局域网接收状态", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        ImGui::Text("传输协议:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "NDI 6 (mDNS 零配置自动发现)");

        ImGui::Text("接收格式:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.1f, 1.0f, 0.5f, 1.0f), "BGRA 32bit (DirectX 11 显存直连)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (config.isConnected) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[● NDI 信号源已接入]");
            if (config.clientIp[0] != '\0') {
                ImGui::Text("当前设备:");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", config.clientIp);
            }
            if (config.streamWidth > 0 && config.streamHeight > 0) {
                ImGui::Text("画面分辨率: %d x %d", config.streamWidth, config.streamHeight);
            }
            ImGui::Text("已接收帧数: %llu 帧", config.totalFramesReceived);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "[○ 等待 NDI 源上线...]");
            ImGui::TextDisabled("自动监听中：支持安卓自研端与苹果【畅想NDI】");
            ImGui::Spacing();
            ImGui::TextWrapped("提示: 只要手机和电脑在同一个 Wi-Fi 下，手机开启推流电脑即可秒级自动连接，无需手动输入 IP。");
        }
        ImGui::Spacing();
    }

    ImGui::Spacing();

    // 卡片 2: 实时性能与延迟监控
    if (ImGui::CollapsingHeader("实时性能与延迟监控", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        if (config.isConnected && config.currentFps > 0.1f) {
            if (config.isH264Udp) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "通道: 私有 UDP H.264  (%.1f Mbps)", config.h264UdpMbps);
            } else {
                ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "通道: NDI (Full/SpeedHQ)");
            }
            ImGui::Text("接收帧率 (PC 实测): ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "%.1f FPS", config.currentFps);

            if (config.hasPhoneStats) {
                ImGui::Text("手机发送帧率 (手机实测): ");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "%.1f FPS", config.phoneFps);
            }

            ImGui::Text("D3D11 呈现帧率: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "%.1f FPS", config.renderFps);

            ImGui::Separator();
            ImGui::Text("实测耗时分解 (全部真实测量):");
            if (config.hasPhoneStats) {
                ImGui::Text("  [手机] 采集帧间隔    : %.2f ms", config.phoneIntervalMs);
                ImGui::Text("  [手机] 取帧          : %.2f ms", config.phoneAcquireMs);
                ImGui::Text("  [手机] RGBA->NDI 发送: %.2f ms", config.phoneSendMs);
            } else {
                ImGui::TextDisabled("  [手机] 等待 metadata 上报...");
            }
            ImGui::Text("  [PC]  NDI 抓帧+拷贝  : %.2f ms", config.pcRecvMs);
            ImGui::Text("  [PC]  显存上传       : %.2f ms", config.pcUploadMs);
            ImGui::Text("  [PC]  绘制 / 呈现    : %.2f / %.2f ms", config.pcDrawMs, config.pcPresentMs);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                "  已实测各段合计: %.2f ms (不含网络段)", config.measuredSumMs);
            ImGui::TextDisabled("  说明: 手机<->PC 时钟未同步, 真·端到端需时钟同步, 故不显示估计值");

            ImGui::Separator();
            ImGui::Text("NDI 官方诊断 (判断瓶颈在谁):");
            ImGui::Text("  累计送达 %lld 帧 | 累计丢弃 %lld 帧 | 队列 %d",
                static_cast<long long>(config.ndiDelivered),
                static_cast<long long>(config.ndiDropped),
                config.ndiQueue);
            if (config.ndiDropped > 0) {
                ImGui::TextDisabled("  丢弃>0: NDI 收到但本端消费不及 → PC 端瓶颈");
            } else {
                ImGui::TextDisabled("  丢弃≈0 仍低帧率: 帧未送达 → 网络/发送端瓶颈");
            }

            if (!config.fpsHistory.empty()) {
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%.1f FPS", config.currentFps);
                ImGui::PlotLines("##FpsPlot", config.fpsHistory.data(),
                                 static_cast<int>(config.fpsHistory.size()),
                                 0, overlay, 0.0f, 144.0f, ImVec2(-1, 65));
            }
        } else {
            ImGui::TextDisabled("NDI 帧率: -- FPS (未推流)");
        }
        ImGui::Spacing();
    }

    ImGui::Spacing();

    // 卡片 3: 画面辅助设置
    if (ImGui::CollapsingHeader("画面辅助与准星", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        ImGui::Checkbox("显示右上角性能 HUD", &config.showHud);
        ImGui::Checkbox("显示中心十字准星", &config.showCrosshair);
        ImGui::Checkbox("显示中心高亮区域", &config.enableCenterRoiOnly);

        if (config.enableCenterRoiOnly) {
            ImGui::SliderInt("区域占比 (%)", &config.roiPercent, 20, 80);
        }
        ImGui::Spacing();
    }

    ImGui::Spacing();

    // 卡片 4: PC 端优化控制 (NDI 接收带宽 + 延迟链 CSV 日志)
    if (ImGui::CollapsingHeader("PC 端优化控制", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        ImGui::Text("NDI 接收带宽请求:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f),
                           "%s", config.bandwidth <= 0 ? "lowest" :
                                 (config.bandwidth >= 100 ? "highest" : "自定义"));
        if (ImGui::SliderInt("##ndi_bandwidth", &config.bandwidth, 0, 100, "带宽 %d")) {
            config.requestApplyBandwidth = true;
        }
        ImGui::TextDisabled("lowest 会连带让发送端降分辨率换帧率；改后自动重连生效");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(config.perfRecording ? "停止并导出性能日志" : "录制性能日志 (CSV)", ImVec2(-1, 0))) {
            config.requestTogglePerfLog = true;
        }
        if (config.perfRecording) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "● 录制中... 再次点击导出 huilang_perf_*.csv");
        } else {
            ImGui::TextDisabled("导出每帧 抓帧/上传/绘制/呈现 耗时");
        }
        ImGui::Spacing();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("技术栈: NDI 6 + DirectX 11 | 广播级超低延迟架构");
}

void StreamingUI::RenderMainView(StreamConfig& config, void* videoTextureSrv, int videoW, int videoH) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + avail.x, p0.y + avail.y);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // 绘制深空暗黑视口背板
    drawList->AddRectFilled(p0, p1, IM_COL32(10, 13, 18, 255));
    drawList->AddRect(p0, p1, IM_COL32(28, 36, 48, 255), 4.0f);

    if (config.isConnected && videoTextureSrv != nullptr && videoW > 0 && videoH > 0) {
        // 真实视频画面渲染：按比例适配视口并居中展示
        float aspect = static_cast<float>(videoW) / static_cast<float>(videoH);
        float dispW = avail.x;
        float dispH = dispW / aspect;
        if (dispH > avail.y) {
            dispH = avail.y;
            dispW = dispH * aspect;
        }
        ImVec2 imgPos = ImVec2(p0.x + (avail.x - dispW) * 0.5f, p0.y + (avail.y - dispH) * 0.5f);
        ImGui::SetCursorScreenPos(imgPos);
        ImGui::Image(reinterpret_cast<ImTextureID>(videoTextureSrv), ImVec2(dispW, dispH));

        ImVec2 center = ImVec2(p0.x + avail.x * 0.5f, p0.y + avail.y * 0.5f);

        // 可选：中心区域高亮框
        if (config.enableCenterRoiOnly) {
            float roiW = dispW * (config.roiPercent / 100.0f);
            float roiH = dispH * (config.roiPercent / 100.0f);
            ImVec2 roiMin = ImVec2(center.x - roiW * 0.5f, center.y - roiH * 0.5f);
            ImVec2 roiMax = ImVec2(center.x + roiW * 0.5f, center.y + roiH * 0.5f);

            drawList->AddRect(roiMin, roiMax, IM_COL32(0, 210, 211, 200), 4.0f, 0, 1.5f);
            drawList->AddText(ImVec2(roiMin.x + 8, roiMin.y + 8), IM_COL32(0, 210, 211, 240), "[中心聚焦区域]");
        }

        // 可选：十字准星辅助
        if (config.showCrosshair) {
            const float crosshairSize = 14.0f;
            drawList->AddLine(ImVec2(center.x - crosshairSize, center.y),
                              ImVec2(center.x + crosshairSize, center.y),
                              IM_COL32(0, 255, 128, 220), 1.5f);
            drawList->AddLine(ImVec2(center.x, center.y - crosshairSize),
                              ImVec2(center.x, center.y + crosshairSize),
                              IM_COL32(0, 255, 128, 220), 1.5f);
            drawList->AddCircle(center, 4.0f, IM_COL32(0, 255, 128, 200), 16, 1.2f);
        }
    } else {
        // 未接入时的清晰引导界面
        float centerY = p0.y + avail.y * 0.40f;

        const char* title = "Castor NDI 极速串流接收器已就绪";
        ImVec2 titleSize = ImGui::CalcTextSize(title);
        drawList->AddText(ImVec2(p0.x + (avail.x - titleSize.x) * 0.5f, centerY),
                          IM_COL32(0, 210, 211, 255), title);

        const char* sub1 = "正在自动监听局域网 NDI 发送源...";
        ImVec2 sub1Size = ImGui::CalcTextSize(sub1);
        drawList->AddText(ImVec2(p0.x + (avail.x - sub1Size.x) * 0.5f, centerY + 36.0f),
                          IM_COL32(220, 230, 240, 230), sub1);

        const char* sub2 = "支持安卓端 Castor App 或苹果端【畅想 NDI】";
        ImVec2 sub2Size = ImGui::CalcTextSize(sub2);
        drawList->AddText(ImVec2(p0.x + (avail.x - sub2Size.x) * 0.5f, centerY + 70.0f),
                          IM_COL32(130, 150, 170, 200), sub2);

        const char* sub3 = "保证手机与电脑在同一 Wi-Fi 下，手机开启广播即可自动出画";
        ImVec2 sub3Size = ImGui::CalcTextSize(sub3);
        drawList->AddText(ImVec2(p0.x + (avail.x - sub3Size.x) * 0.5f, centerY + 98.0f),
                          IM_COL32(0, 255, 128, 220), sub3);
    }
}

void StreamingUI::RenderHudOverlay(const StreamConfig& config) {
    if (!config.isConnected) return;

    ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration |
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoNav;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 screenPos = ImGui::GetWindowPos();
    ImGui::SetNextWindowPos(ImVec2(screenPos.x + avail.x - 260.0f, screenPos.y + 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.70f);

    ImGui::Begin("LatencyHUD", nullptr, hudFlags);
    ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "Castor NDI 极速串流");
    ImGui::Separator();
    if (config.clientIp[0] != '\0') {
        ImGui::Text("信号源: %s", config.clientIp);
    }
    ImGui::Text("接收帧率(PC): %.1f FPS", config.currentFps);
    if (config.hasPhoneStats) {
        ImGui::Text("发送帧率(手机): %.1f FPS", config.phoneFps);
    }
    ImGui::TextColored(ImVec4(0.1f, 1.0f, 0.5f, 1.0f), "实测合计: %.2f ms", config.measuredSumMs);
    ImGui::TextDisabled("  ├ 手机 发送: %.2f ms", config.phoneSendMs);
    ImGui::TextDisabled("  └ PC 接->呈: %.2f ms",
        config.pcRecvMs + config.pcUploadMs + config.pcDrawMs + config.pcPresentMs);
    if (config.streamWidth > 0 && config.streamHeight > 0) {
        ImGui::TextDisabled("规格: %d x %d", config.streamWidth, config.streamHeight);
    }
    ImGui::End();
}

} // namespace CastorUI
