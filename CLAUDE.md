# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## 项目概览

**Castor** 是一套极低延迟的 Android → Windows 游戏投屏/串流系统，目标端到端延迟 15~30ms、60/120fps。设计蓝图见根目录 `架构设计方案.md`（PC 端 D3D11 + CUDA 零拷贝 AI 推理规划）。

仓库由两个独立工程组成，通过**网络协议**耦合，二者之间没有共享代码：

- `Castor-Desktop/` — Windows 接收端。C++20 + Direct3D 11 + Dear ImGui，Clang-cl + CMake + Ninja 构建。
- `Castor-Mobile/` — Android 发送端。Kotlin（UI/采集/编码调度）+ NDK C++（JNI 编码与 NDI 发送）。Gradle 构建，仅 `arm64-v8a`。
- 根目录另有 NDI SDK 安装包、`install_app.bat`、`启动Castor电脑端.bat` 等便捷脚本。

## 构建与运行

### Desktop（Windows）

```bat
Castor-Desktop\build.bat            # 内部调用 build.ps1
powershell -ExecutionPolicy Bypass -File Castor-Desktop\build.ps1   # 等价入口
启动Castor电脑端.bat                 # 直接运行已编译的 exe
```

产物：`D:\Castor\Castor-Desktop\build\CastorDesktop.exe`

**重要**：`build.ps1` 内写死了一套本机工具链绝对路径，换机器必须改：
- VS 环境 `G:\vis1\VC\Auxiliary\Build\vcvars64.bat`
- CMake `G:\cmake\cmake-3.29.2-windows-x86_64\bin`
- Clang `D:\llvm20\bin`，Python `G:\python3.11.9\Scripts`

脚本会先 `Stop-Process CastorDesktop`（避免 lld-link 写锁），再用 `clang-cl` + Ninja 编译。第三方库在 `Castor-Desktop/third_party/`：`imgui/`（源码直接编译）、`turbojpeg/`、`ndi/`（含 x64 静态库与运行时 DLL，构建后自动拷贝 DLL 到输出目录）。

### Mobile（Android）

```bat
install_app.bat                     # 编译 debug APK + adb install + 自动启动 MainActivity
Castor-Mobile\gradlew.bat assembleDebug
```

- `applicationId` / namespace = `io.github.xiangsu1145.castor`，`minSdk 31`。
- `compileSdk 36`，NDK CMake 3.22.1。
- 预编译原生库**必须手工就位**（不在 CMake 里重新构建）：
  - `app/src/main/jniLibs/arm64-v8a/libndi.so`、`libturbojpeg.so`（运行时动态库）
  - `app/src/main/cpp/third_party/prebuilt/arm64-v8a/libturbojpeg.a`（静态库，NEON 版）+ `prebuilt/include/turbojpeg.h`
  - NDK C++ 目标名 `screenbridge`，源文件 `app/src/main/cpp/screenbridge.cpp`。

## 架构：数据如何流动

### 发送端（Android）

`MainActivity`（Material3 界面，仅负责 MediaProjection 授权与参数）→ 绑定 `ScreenCaptureService`（前台服务）→ **当前实际链路走 NDI**：

```
MediaProjection → VirtualDisplay → ImageReader(RGBA_8888, 2 buffers)
   → ScreenBridge.nativeNdiSend  (screenbridge.cpp, JNI)
        → libturbojpeg tjEncodeYUV3  RGBA→I420 (NEON)
        → NDIlib_send_send_video_async_v2  (clock_video=false, 零休眠直推)
   → NDI 内部线程做 NDI 协议封装/发送
```

伴随一个 UDP 5990 广播信标线程（`CASTOR_NDI_BEACON:<name>:<ip>`），用于穿透 AP 组播隔离、让 PC 端直接单播探测手机 IP。全程持有 WiFi `multicastLock` + `MULTICAST_STATE`/`LOW_LATENCY` wifiLock。

**关键陷阱**：`ScreenCaptureService.startStreaming(ip, port, ...)` 的参数名是历史遗留——`ip` 实际承载的是 **NDI 源名称**，`port` 传 0 且被忽略。`MainActivity` 第 ~97 行就是这么调的，改动时不要按字段名理解语义。

仓库里还留有**未接入主链路**的旧实现，阅读时勿混淆：`ScreenStreamer`（MediaCodec H264 → TCP）、`H264UdpStreamer` / `UdpSender` / `StreamProtocol`（H264 UDP 分片）、`ScreenBridge.nativeEncode`（裁剪 + MJPEG，配合桌面端 split-JPEG 路径）、`net/LanDiscovery`、`net/ClockSync`。`screenbridge.cpp` 同时编译了 MJPEG 与 NDI 两套 JNI，NDI 为当前使用项。

### 接收端（Desktop）

`main.cpp` 的 `wWinMain` 在单个 `while(running)` 消息循环里**同时启动并轮询三条接收路径**，谁先产生新帧就用谁：

1. `Receiver`（`mirror_receiver.cpp`，TCP 分块 JPEG / "灰狼"路径）— 二进制帧头走大端序（`mirror_net.cpp` 的 `ReadBE32/64`），魔数 `0x484C4D32`，多核分块并行解码。`mirror_types.h` 的 `SharedState` / `DecodedFrame` 是其状态与统计载体。
2. `VideoReceiver`（`video_receiver.cpp`，**实为 NDI 接收器**，虽命名/注释写着 H.264 UDP）— UDP 5990 收信标 → NDI Finder 发现源 → `NDIlib_recv_create_v3`（`color_format = BGRX_BGRA` 直出 BGRA）。抓帧后**排空积压帧**只留最新一帧，超时 20ms。
3. `NetworkServer`（`network_server.cpp`）— UDP 广播发现 + 本机 IP 上报，`StartBeacon(27183)`。

渲染：新帧经 `Map(D3D11_MAP_WRITE_DISCARD)` 逐行 `memcpy` 上传到一张 `DXGI_FORMAT_B8G8R8A8_UNORM` 动态纹理，`StreamingUI::Render` 把该 SRV 作为全屏贴图绘出，叠加 ImGui HUD（FPS/延迟拆解曲线）。`Present(0, 0)` 关闭垂直同步。

配置常量集中在 `mirror_types.h`：`PORT=27183`、`AUDIO_PORT=27184`、`CENTER_ROI_VIDEO_PORT=27185`、NDI 信标 `5990`。

### 辅助模块（Desktop）

- `h264_decoder.cpp` — 基于 Media Foundation `CLSID_CMSH264DecoderMFT` 的 H.264 软解，开启 `CODECAPI_AVLowLatencyMode`，NV12→BGRA 纯 CPU 转换（`ConvertNV12ToBGRA_Fast`）。供 center-ROI 混合路径使用。
- `mirror_net.cpp` — ADB `forward/reverse` 端口映射、socket 低延迟配置（`TCP_NODELAY`、增大缓冲）、大端序读写。
- `perf_logger.cpp` / `hl_common.cpp` — 性能打点与公共工具。
- `streaming_ui.cpp` / `theme.cpp` — ImGui 控制台界面与 **Deep Cyber Slate** 皮肤、中文字体加载（优先 `msyh.ttc`，回退黑体/宋体）。

## 性能敏感点（改动时务必注意）

这是全项目的核心目标，任何"顺手"的分配或拷贝都可能毁掉延迟预算：

- 发送端 `NdiStreamer` 用 `isSending` 原子标志**主动丢帧**：上一帧未发完就丢弃新帧，绝不排队。桌面端 NDI 接收同理（排空积压帧）。这是刻意的反缓冲累积设计，不要改成队列。
- `screenbridge.cpp` 所有缓冲区（`g_yuvPing/g_yuvPong`、`g_jpegBuf`）**预分配复用，逐帧零 malloc**；NDI 用 Ping-Pong 双缓冲保证 `send_video_async_v2` 异步期间内存安全。改编码路径时保持这个契约。
- 桌面 `main.cpp` 每帧对全帧做两次 CPU 全量拷贝（NDI 帧 → `FrameBuffer::rgba`，再 → D3D11 动态纹理）。这是已知的 CPU 热点，也是低帧率常见根因。
- 发送端 `VirtualDisplay → ImageReader(RGBA_8888)` 意味着每帧都要 **GPU→CPU 回读 4B/px 的 RGBA**，再做 RGBA→I420 软件转换——这是发送端的 CPU 瓶颈来源。要做到 60/120fps 通常需要改走 MediaCodec 硬件编码（见 `架构设计方案.md` 方案 B）或 GPU 侧转换。
- `SharedState` / `latestFrame_` 等跨线程传输统一用"最新帧覆盖"语义 + mutex 保护，不累积历史。

## 参考

- `架构设计方案.md` — 完整设计指标、D3D11 Flip 模型配置、CUDA 零拷贝互操作、TurboJPEG 分块编码、uinput 触控等方案的权威来源。
- `Castor-Desktop/README.md` — 桌面端特性与编译说明。
