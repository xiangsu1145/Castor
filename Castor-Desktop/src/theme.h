#pragma once
#include <imgui.h>

namespace CastorUI {

// 加载中文字体（优先使用微软雅黑，回退黑体/宋体）
bool LoadChineseFont(ImGuiIO& io, float fontSize = 17.0f);

// 设置高颜值现代化暗黑/电竞科技蓝主题
void ApplyModernDarkTheme();

} // namespace CastorUI
