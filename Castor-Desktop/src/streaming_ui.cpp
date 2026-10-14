#include "streaming_ui.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace CastorUI {

StreamingUI::StreamingUI() {
}

void StreamingUI::Render(StreamConfig& config, float screenWidth, float screenHeight,
                         void* videoTextureSrv, int videoW, int videoH,
                         void* roiTextureSrv, int roiW, int roiH) {
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
    RenderMainView(config, videoTextureSrv, videoW, videoH, roiTextureSrv, roiW, roiH);
    if (config.showHud) {
        RenderHudOverlay(config);
    }
    ImGui::EndChild();

    ImGui::End();
}

void StreamingUI::RenderSidebar(StreamConfig& config) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.00f, 0.86f, 0.88f, 1.0f), "Castor 投屏接收端");
    ImGui::TextDisabled("版本: 0.0.1 | 渲染引擎: DirectX 11");
    ImGui::Separator();
    ImGui::Spacing();

    // 卡片 1: 接收模式与设备连接
    if (ImGui::CollapsingHeader("接收设置与连接", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        ImGui::Text("画面接收模式:");

        bool modeChanged = false;
        if (ImGui::RadioButton("UDP 模式 (默认 / 低延迟)", config.receiveMode == 1)) {
            config.receiveMode = 1;
            modeChanged = true;
        }
        if (ImGui::RadioButton("NDI 模式 (兼容模式)", config.receiveMode == 2)) {
            config.receiveMode = 2;
            modeChanged = true;
        }
        if (ImGui::RadioButton("自动检测", config.receiveMode == 0)) {
            config.receiveMode = 0;
            modeChanged = true;
        }

        if (modeChanged) {
            config.requestResetStream = true;
        }

        ImGui::Spacing();
        if (config.receiveMode == 1) {
            ImGui::TextDisabled("说明: Castor 专属高速低延迟协议，支持中心原画。");
        } else if (config.receiveMode == 2) {
            ImGui::TextDisabled("说明: 通用投屏协议，兼容第三方 NDI 格式发送端。");
        } else {
            ImGui::TextDisabled("说明: 优先接收 UDP 流，若无则自动接收 NDI 流。");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("本机 IP 地址: %s", config.hostIp);
        ImGui::Text("接收监听端口: 27190 (UDP) / 27183 (NDI)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (config.isConnected) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[● 投屏已连接]");
            if (config.clientIp[0] != '\0') {
                ImGui::Text("来源设备: %s", config.clientIp);
            }
            if (config.streamWidth > 0 && config.streamHeight > 0) {
                ImGui::Text("画面分辨率: %d x %d", config.streamWidth, config.streamHeight);
            }
            ImGui::Text("已接收帧数: %llu 帧", config.totalFramesReceived);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "[○ 等待投屏连接...]");
            ImGui::TextDisabled("提示: 确保手机和电脑在同一 Wi-Fi 下，手机点击【启动无线投屏】即可自动连接。");
        }

        ImGui::Spacing();
        if (ImGui::Button("刷新 / 重新等待连接", ImVec2(-1, 30))) {
            config.requestResetStream = true;
        }
        if (config.isConnected) {
            ImGui::Spacing();
            if (ImGui::Button("断开当前画面", ImVec2(-1, 26))) {
                config.requestDisconnect = true;
            }
        }
        ImGui::Spacing();
    }

    ImGui::Spacing();

    // 卡片 2: 画面与运行状态
    if (ImGui::CollapsingHeader("运行状态", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        if (config.isConnected && config.currentFps > 0.1f) {
            if (config.isH264Udp) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "当前协议: UDP 模式 (%.1f Mbps)", config.h264UdpMbps);
            } else {
                ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "当前协议: NDI 模式");
            }
            ImGui::Text("接收帧率: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "%.1f FPS", config.currentFps);

            ImGui::Text("界面渲染帧率: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "%.1f FPS", config.renderFps);

            ImGui::Text("渲染耗时: %.2f ms", config.renderMs);

            if (!config.fpsHistory.empty()) {
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%.1f FPS", config.currentFps);
                ImGui::PlotLines("##FpsPlot", config.fpsHistory.data(),
                                 static_cast<int>(config.fpsHistory.size()),
                                 0, overlay, 0.0f, 144.0f, ImVec2(-1, 65));
            }

            if (config.hasRoiStream) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "● 屏幕中心高清区域");
                ImGui::Text("  原画规格: %dx%d (18 Mbps 高码率)", config.roiWidth, config.roiHeight);
                ImGui::Text("  原画帧率: %.1f FPS", config.roiFps);
                ImGui::Checkbox("显示中心原画定位框", &config.showRoiBoundary);
            }
        } else {
            ImGui::TextDisabled("当前未推流，暂无运行数据");
        }
        ImGui::Spacing();
    }

    ImGui::Spacing();

    // 卡片 3: 画面辅助设置
    if (ImGui::CollapsingHeader("画面辅助与准星", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        ImGui::Checkbox("显示右上角状态浮窗 (HUD)", &config.showHud);
        ImGui::Checkbox("显示中心十字准星", &config.showCrosshair);
        ImGui::Checkbox("高亮屏幕中心区域", &config.enableCenterRoiOnly);

        if (config.enableCenterRoiOnly) {
            ImGui::SliderInt("高亮区域大小 (%)", &config.roiPercent, 20, 80);
        }
        ImGui::Spacing();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Castor 无线投屏工具");
}

void StreamingUI::RenderMainView(StreamConfig& config, void* videoTextureSrv, int videoW, int videoH,
                                 void* roiTextureSrv, int roiW, int roiH) {
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

        // 中心原画 (Picture-in-Picture) 居中 1:1 物理比例覆盖渲染
        if (roiTextureSrv != nullptr && roiW > 0 && roiH > 0) {
            float roiDispW = dispW * (static_cast<float>(roiW) / static_cast<float>(videoW));
            float roiDispH = dispH * (static_cast<float>(roiH) / static_cast<float>(videoH));
            ImVec2 roiPos = ImVec2(center.x - roiDispW * 0.5f, center.y - roiDispH * 0.5f);
            ImVec2 roiEnd = ImVec2(roiPos.x + roiDispW, roiPos.y + roiDispH);

            drawList->AddImage(
                reinterpret_cast<ImTextureID>(roiTextureSrv),
                roiPos, roiEnd
            );

            if (config.showRoiBoundary) {
                drawList->AddRect(roiPos, roiEnd, IM_COL32(0, 210, 211, 180), 0.0f, 0, 1.2f);
                drawList->AddText(ImVec2(roiPos.x + 6, roiPos.y + 6), IM_COL32(0, 210, 211, 220), "[中心原画]");
            }
        }

        // 可选：中心区域高亮框
        if (config.enableCenterRoiOnly) {
            float roiW = dispW * (config.roiPercent / 100.0f);
            float roiH = dispH * (config.roiPercent / 100.0f);
            ImVec2 roiMin = ImVec2(center.x - roiW * 0.5f, center.y - roiH * 0.5f);
            ImVec2 roiMax = ImVec2(center.x + roiW * 0.5f, center.y + roiH * 0.5f);

            drawList->AddRect(roiMin, roiMax, IM_COL32(0, 210, 211, 200), 4.0f, 0, 1.5f);
            drawList->AddText(ImVec2(roiMin.x + 8, roiMin.y + 8), IM_COL32(0, 210, 211, 240), "[中心高亮区域]");
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
        float centerY = p0.y + avail.y * 0.38f;

        const char* title = "等待投屏连接...";
        ImVec2 titleSize = ImGui::CalcTextSize(title);
        drawList->AddText(ImVec2(p0.x + (avail.x - titleSize.x) * 0.5f, centerY),
                          IM_COL32(0, 210, 211, 255), title);

        const char* sub1 = "请确保手机与电脑连接在同一局域网 (Wi-Fi)";
        ImVec2 sub1Size = ImGui::CalcTextSize(sub1);
        drawList->AddText(ImVec2(p0.x + (avail.x - sub1Size.x) * 0.5f, centerY + 36.0f),
                          IM_COL32(220, 230, 240, 230), sub1);

        const char* sub2 = "在手机端打开 Castor，点击【启动无线投屏】即可自动出画";
        ImVec2 sub2Size = ImGui::CalcTextSize(sub2);
        drawList->AddText(ImVec2(p0.x + (avail.x - sub2Size.x) * 0.5f, centerY + 68.0f),
                          IM_COL32(130, 150, 170, 200), sub2);

        const char* sub3 = "支持 UDP 低延迟通道与 NDI 兼容模式";
        ImVec2 sub3Size = ImGui::CalcTextSize(sub3);
        drawList->AddText(ImVec2(p0.x + (avail.x - sub3Size.x) * 0.5f, centerY + 96.0f),
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
    ImGui::SetNextWindowPos(ImVec2(screenPos.x + avail.x - 240.0f, screenPos.y + 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.70f);

    ImGui::Begin("LatencyHUD", nullptr, hudFlags);
    ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "Castor 投屏");
    ImGui::Separator();
    if (config.clientIp[0] != '\0') {
        ImGui::Text("设备: %s", config.clientIp);
    }
    ImGui::Text("协议: %s", config.isH264Udp ? "UDP 低延迟" : "NDI 兼容");
    ImGui::Text("帧率: %.1f FPS", config.currentFps);
    if (config.streamWidth > 0 && config.streamHeight > 0) {
        ImGui::Text("规格: %d x %d", config.streamWidth, config.streamHeight);
    }
    ImGui::Text("渲染: %.2f ms", config.renderMs);
    ImGui::End();
}

} // namespace CastorUI
