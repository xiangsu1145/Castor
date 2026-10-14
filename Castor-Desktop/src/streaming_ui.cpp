#include "streaming_ui.h"
#include "model_manager.h"
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

    // 渲染导入 ONNX 模型弹窗 (若触发)
    RenderImportModelModal();

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

    // 卡片 4: AI 模型管理与参数配置
    RenderModelManagementSection();

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

void StreamingUI::RenderModelManagementSection() {
    if (ImGui::CollapsingHeader("AI 模型管理 (ONNX)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Spacing();
        auto& mgr = CastorAI::ModelManager::Instance();
        auto& models = mgr.GetModels();
        auto* activeModel = mgr.GetActiveModel();

        // 导入模型按钮
        if (ImGui::Button("+ 导入 ONNX 模型", ImVec2(-1, 30))) {
            mgr.TriggerImportFlow();
        }

        ImGui::Spacing();
        ImGui::Text("选择当前生效模型:");

        // 模型选择下拉框 (Combo)
        std::string currentModelLabel = activeModel ? (activeModel->name + " (" + std::to_string(activeModel->width) + "x" + std::to_string(activeModel->height) + ")") : "暂无可用模型 (请先导入)";
        if (ImGui::BeginCombo("##ModelSelectCombo", currentModelLabel.c_str())) {
            for (const auto& m : models) {
                bool isSelected = (activeModel && activeModel->id == m.id);
                std::string label = m.name + " (" + std::to_string(m.width) + "x" + std::to_string(m.height) + ")";
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    mgr.SetActiveModelId(m.id);
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (activeModel) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("模型名称: %s", activeModel->name.c_str());
            ImGui::Text("输入分辨率: %d x %d", activeModel->width, activeModel->height);
            ImGui::TextDisabled("缓存路径: %s", activeModel->relativePath.c_str());

            ImGui::Spacing();
            // 全局总 NMS (默认 0.45)
            if (ImGui::SliderFloat("总 NMS 阈值", &activeModel->globalNms, 0.10f, 0.95f, "%.2f")) {
                mgr.SaveModels();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("类别专属阈值配置 (%zu 个类别):", activeModel->classes.size());

            if (!activeModel->classes.empty()) {
                static int s_selectedClassIdx = 0;
                if (s_selectedClassIdx >= (int)activeModel->classes.size()) {
                    s_selectedClassIdx = 0;
                }

                // 类别下拉选择框 (Combo)
                std::string curClassLabel = "[" + std::to_string(activeModel->classes[s_selectedClassIdx].id) + "] " + activeModel->classes[s_selectedClassIdx].name;
                if (ImGui::BeginCombo("##ClassSelectCombo", curClassLabel.c_str())) {
                    for (int i = 0; i < (int)activeModel->classes.size(); ++i) {
                        bool isSel = (s_selectedClassIdx == i);
                        std::string clsItemLabel = "[" + std::to_string(activeModel->classes[i].id) + "] " + activeModel->classes[i].name;
                        if (ImGui::Selectable(clsItemLabel.c_str(), isSel)) {
                            s_selectedClassIdx = i;
                        }
                        if (isSel) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                auto& curClass = activeModel->classes[s_selectedClassIdx];
                ImGui::Spacing();
                // 类别专属置信度与 IOU 设置 (默认 0.50)
                if (ImGui::SliderFloat("类别置信度 (Conf)", &curClass.conf, 0.05f, 0.95f, "%.2f")) {
                    mgr.SaveModels();
                }
                if (ImGui::SliderFloat("类别交并比 (IOU)", &curClass.iou, 0.05f, 0.95f, "%.2f")) {
                    mgr.SaveModels();
                }

                ImGui::Spacing();
                if (ImGui::Button("同步此参数到全部类别", ImVec2(-1, 24))) {
                    for (auto& c : activeModel->classes) {
                        c.conf = curClass.conf;
                        c.iou = curClass.iou;
                    }
                    mgr.SaveModels();
                }
            } else {
                ImGui::TextDisabled("该模型暂无类别信息");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("删除此模型", ImVec2(-1, 24))) {
                mgr.DeleteModel(activeModel->id);
            }
        } else {
            ImGui::Spacing();
            ImGui::TextDisabled("提示: 暂无导入的模型。点击上方【+ 导入 ONNX 模型】即可选择 .onnx 文件导入。");
        }
        ImGui::Spacing();
    }
}

void StreamingUI::RenderImportModelModal() {
    auto& mgr = CastorAI::ModelManager::Instance();
    auto& pending = mgr.GetPendingImport();

    if (pending.isOpen) {
        ImGui::OpenPopup("导入 ONNX 模型确认##Modal");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("导入 ONNX 模型确认##Modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("正在导入模型:");
        ImGui::TextColored(ImVec4(0.0f, 0.86f, 0.88f, 1.0f), "%s", pending.originalFilePath.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("模型名称 (必填):");
        ImGui::InputText("##ModelNameInput", pending.modelNameInput, sizeof(pending.modelNameInput));

        ImGui::Spacing();
        ImGui::Text("输入分辨率: %d x %d (已自动解析)", pending.detectedWidth, pending.detectedHeight);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (pending.hasMetadata && !pending.detectedClasses.empty()) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "● 成功从 ONNX 元数据解析到 %zu 个类别:", pending.detectedClasses.size());
            ImGui::BeginChild("DetectedClassesPreview", ImVec2(0, 110), true);
            for (size_t i = 0; i < pending.detectedClasses.size(); ++i) {
                ImGui::Text("[%zu] %s", i, pending.detectedClasses[i].c_str());
            }
            ImGui::EndChild();
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "● 未在 ONNX 元数据中检测到类别信息");
            ImGui::TextWrapped("请在下方手动输入类别名称（每行一个类别，如 body、head）：");
            ImGui::InputTextMultiline("##ManualClasses", pending.manualClassesText, sizeof(pending.manualClassesText), ImVec2(-1, 100));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool canConfirm = (pending.modelNameInput[0] != '\0');
        if (!canConfirm) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("确认导入", ImVec2(120, 32))) {
            mgr.FinalizeImport(pending);
            pending.isOpen = false;
            ImGui::CloseCurrentPopup();
        }
        if (!canConfirm) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 32))) {
            pending.isOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace CastorUI
