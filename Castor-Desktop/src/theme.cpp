#include "theme.h"
#include <windows.h>
#include <vector>
#include <string>

namespace CastorUI {

bool LoadChineseFont(ImGuiIO& io, float fontSize) {
    // 候选字体列表（优先无衬线现代中文字体）
    const char* fontCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",   // 微软雅黑
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simhei.ttf", // 黑体
        "C:\\Windows\\Fonts\\simsun.ttc"  // 宋体
    };

    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = true;

    for (const char* path : fontCandidates) {
        DWORD attrib = GetFileAttributesA(path);
        if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
            // 加载完整中文字符集
            ImFont* font = io.Fonts->AddFontFromFileTTF(
                path,
                fontSize,
                &config,
                io.Fonts->GetGlyphRangesChineseFull()
            );
            if (font) {
                return true;
            }
        }
    }

    // 兜底：如果系统字体加载失败，加载内置默认字体
    io.Fonts->AddFontDefault();
    return false;
}

void ApplyModernDarkTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // ==========================================
    // 基础几何与间距风格微调 (流线扁平化 + 柔和圆角)
    // ==========================================
    style.WindowPadding     = ImVec2(16.0f, 16.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.CellPadding       = ImVec2(8.0f, 5.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing     = 20.0f;
    style.ScrollbarSize     = 13.0f;
    style.GrabMinSize       = 10.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.5f;
    style.TabBorderSize     = 0.0f;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;
    style.LogSliderDeadzone = 4.0f;
    style.TabRounding       = 5.0f;

    // ==========================================
    // 精调色彩体系：Deep Cyber Slate (深海极客蓝)
    // ==========================================
    // 文本色
    colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.53f, 0.62f, 1.00f);

    // 窗口背景 (深度暗蓝灰，富有层次感)
    colors[ImGuiCol_WindowBg]              = ImVec4(0.07f, 0.09f, 0.12f, 0.96f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.09f, 0.12f, 0.16f, 0.70f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.13f, 0.18f, 0.98f);

    // 边框色
    colors[ImGuiCol_Border]                = ImVec4(0.18f, 0.24f, 0.32f, 0.85f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // 控件框架 (输入框、下拉框、复选框背景)
    colors[ImGuiCol_FrameBg]               = ImVec4(0.12f, 0.16f, 0.22f, 0.85f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.16f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.20f, 0.28f, 0.38f, 1.00f);

    // 标题栏
    colors[ImGuiCol_TitleBg]               = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.08f, 0.11f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.06f, 0.08f, 0.11f, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.09f, 0.12f, 0.16f, 1.00f);

    // 滚动条
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.07f, 0.09f, 0.12f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.22f, 0.29f, 0.38f, 0.90f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.29f, 0.38f, 0.50f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.00f, 0.78f, 0.82f, 1.00f); // 抓取时高亮青色

    // 关键复选与滑块
    colors[ImGuiCol_CheckMark]             = ImVec4(0.00f, 0.86f, 0.88f, 1.00f); // 亮青色 CheckMark
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.00f, 0.78f, 0.82f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.10f, 0.92f, 0.95f, 1.00f);

    // 按钮 (渐变式主调青蓝)
    colors[ImGuiCol_Button]                = ImVec4(0.12f, 0.38f, 0.52f, 0.65f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.15f, 0.48f, 0.66f, 0.90f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.00f, 0.78f, 0.82f, 1.00f);

    // 折叠卡片 Header
    colors[ImGuiCol_Header]                = ImVec4(0.13f, 0.19f, 0.26f, 0.85f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.18f, 0.26f, 0.36f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.00f, 0.65f, 0.72f, 0.80f);

    // 分隔线
    colors[ImGuiCol_Separator]             = ImVec4(0.18f, 0.24f, 0.32f, 0.70f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.00f, 0.78f, 0.82f, 0.80f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.00f, 0.86f, 0.88f, 1.00f);

    // 调整手柄 ResizeGrip
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.18f, 0.24f, 0.32f, 0.30f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.00f, 0.78f, 0.82f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.00f, 0.86f, 0.88f, 0.95f);

    // 选项卡 Tabs
    colors[ImGuiCol_Tab]                   = ImVec4(0.10f, 0.14f, 0.19f, 0.90f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.17f, 0.27f, 0.38f, 0.95f);
    colors[ImGuiCol_TabSelected]           = ImVec4(0.14f, 0.32f, 0.46f, 1.00f);
    colors[ImGuiCol_TabDimmed]             = ImVec4(0.08f, 0.11f, 0.15f, 0.85f);
    colors[ImGuiCol_TabDimmedSelected]     = ImVec4(0.11f, 0.20f, 0.28f, 0.95f);

    // 绘图表格
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.11f, 0.15f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.15f, 0.20f, 0.27f, 0.70f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    // 导航与聚焦提示
    colors[ImGuiCol_NavHighlight]          = ImVec4(0.00f, 0.86f, 0.88f, 0.80f);
}

} // namespace CastorUI
