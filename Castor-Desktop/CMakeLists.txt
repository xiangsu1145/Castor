cmake_minimum_required(VERSION 3.20)
project(CastorDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 确保 MSVC / Clang 启用 UTF-8 编码
if (MSVC)
    add_compile_options(/utf-8 /permissive-)
endif()

# ImGui 路径
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)

set(APP_SOURCES
    "src/main.cpp"
    "src/theme.h"
    "src/theme.cpp"
    "src/streaming_ui.h"
    "src/streaming_ui.cpp"
    "src/network_server.h"
    "src/network_server.cpp"
    "src/video_receiver.h"
    "src/video_receiver.cpp"
    "src/hl_common.h"
    "src/hl_common.cpp"
    "src/perf_logger.h"
    "src/perf_logger.cpp"
    "src/mirror_net.h"
    "src/mirror_net.cpp"
    "src/mirror_types.h"
    "src/mirror_receiver.h"
    "src/mirror_receiver.cpp"
    "src/h264_decoder.h"
    "src/h264_decoder.cpp"
    "src/h264_udp_receiver.h"
    "src/h264_udp_receiver.cpp"
)

add_executable(CastorDesktop WIN32
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(CastorDesktop PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/turbojpeg/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ndi/include"
)

# 链接 Windows 与 DirectX 11 核心库及网络、TurboJPEG、Media Foundation 库、NDI 核心库
target_link_libraries(CastorDesktop PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    ws2_32
    iphlpapi
    mfplat
    mfuuid
    ole32
    oleaut32
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/turbojpeg/x64/turbojpeg-static.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ndi/lib/x64/Processing.NDI.Lib.x64.lib"
)

# 自动拷贝 NDI 运行时 DLL 到目标输出目录
add_custom_command(TARGET CastorDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ndi/bin/x64/Processing.NDI.Lib.x64.dll"
    "$<TARGET_FILE_DIR:CastorDesktop>"
)

# 优化编译输出
if(MSVC)
    target_compile_options(CastorDesktop PRIVATE /W4)
else()
    target_compile_options(CastorDesktop PRIVATE -Wall -Wextra)
endif()
