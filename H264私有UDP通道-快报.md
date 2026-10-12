# Castor H.264 私有 UDP 投屏通道 — 进度与待办报告

> 用途：本报告自包含。上下文压缩后据此继续。
> 日期：2026-10-12

---

## 1. 目标与背景

做**无线投屏 + 键鼠映射**软件，要求：
- **兼容标准 NDI**（第三方 NDI 接收端能用）+ **自研私有 UDP 通道**（自家 PC 客户端用）。
- 指标：**1080p60 / 端到端 ≈30ms / 不丢包（或可恢复）**。
- **必须无线**：手机 USB 口要留给 ESP32-S3（映射/HID），不能走有线。

**为什么必须自研私有通道**：标准 NDI Full(SpeedHQ) 码率太高。实测 1080×1536@45fps = **101.6 Mbps**，WiFi 顶不住，`nativeNdiSend` 被网络回压卡 15ms/帧，帧率钉死 ~45fps。标准 NDI SDK **不能发 H.264/HX**（要付费的 NDI Advanced SDK）。所以私有通道必须**自己用 MediaCodec 硬编 H.264**。

---

## 2. 已打通的完整链路（当前状态：✅ 出画、颜色正确）

```
【手机】 MainActivity(选通道) → ScreenCaptureService.startUdpStream
        → H264UdpStreamer
             MediaProjection → VirtualDisplay → MediaCodec 硬编 H.264 (输入 Surface, 零回读)
             → UDP 分片直推 PC:27190

【PC】   main.cpp → CastorVideo::H264UdpReceiver (h264_udp_receiver.cpp)
             UDP 收包 → 按 frameSeq 重组帧 → [解码线程] h264_decoder.cpp (MF 软解 NV12)
             → [渲染线程] main.cpp 调 ConvertOnGpu()：NV12 两平面→纹理→pixel shader 转 BGRA→纹理
             → ImGui 直接用该 GPU 纹理的 SRV 显示（无 CPU 上传）
```

**握手/发现**：手机 `LanDiscovery.kt` 向 PC:19889 发 `CASTOR_DISCOVERY_PROBE`，PC `network_server.cpp` 回 `CASTOR_BEACON:名:IP:端口:phoneUs:pcUs`（顺便做时钟同步）。手机自动拿到 PC IP。
**关键帧恢复**：PC 解不出就回 4 字节 `!IDR`，手机 `H264UdpStreamer` 收到后 `setParameters(REQUEST_SYNC_FRAME)` 立刻补 IDR。

---

## 3. 关键参数与实测数据

**手机编码配置**（`H264UdpStreamer.setupCodecAndVirtualDisplay`，来自 H264Probe 实测最优）：
```
KEY_LATENCY = 0
KEY_PRIORITY = 0
KEY_OPERATING_RATE = fps
vendor.qti-ext-enc-low-latency.enable = 1
KEY_BITRATE_MODE = CBR
KEY_I_FRAME_INTERVAL = 1
【不要设】KEY_REPEAT_PREVIOUS_FRAME_AFTER（实测会加长尾延迟）
```

**UDP 线协议**（24 字节头，小端）：
```
0  u32 magic "H264"(0x34363248)   4  u32 frameSeq
8  u16 fragIndex                  10 u16 fragCount
12 u16 len(本片字节)              14 u16 flags(bit0=关键帧)
16 u64 ptsUs                      载荷 ≤1400B
```

**实测**：
| 项 | 值 |
|---|---|
| 分辨率 | 1088×1536（16 对齐后） |
| 手机发送 | 120~126 fps |
| 码率 | 运动 ~18 Mbps / 静止 ~10 Mbps |
| 手机编码延迟 | ~22ms（p50） |
| PC MFT 软解 | ~5.4ms/帧 |
| PC NV12→BGRA | GPU shader（CPU 只剩 0.25ms 拷贝） |

---

## 4. 已修复的问题（踩过的坑，别再犯）

1. **手机 `nativeNdiSend` 阻塞 15ms** → 实为 WiFi 回压（Full NDI 码率太高），非代码问题。
2. **PC 解码器不出帧**（`hrOut=0xC00D6D60`）→ **`MF_E_TRANSFORM_TYPE_NOT_SET`**：设了输入类型但**没设输出类型**。修法：拿到 SPS/PPS 后 `SetInputType(带 MF_MT_MPEG_SEQUENCE_HEADER)` **并调 `ConfigureOutputMediaType()`**。
3. **`MF_E_TRANSFORM_NEED_MORE_INPUT` 误判**：0xC00D6D60 是 TYPE_NOT_SET，不是 NEED_MORE_INPUT。查错码要查头文件。
4. **静止画面不解码**：`I_FRAME_INTERVAL=1` 编码器按"帧数"算 GOP，静止时很久才出 IDR。修法：PC 端 `!IDR` 回传（已实现）。
5. **SSE2 转换颜色错/黑块/锯齿**：`unpackhi_epi8` 取的是第 8-15 字节（重复），不是像素 4-7。改 8 像素/次 + 正确交错。
6. **软件卡死**（上一版）：**D3D11 immediate context 是单线程的**，在解码线程用它做 GPU 转换 → 和渲染线程冲突 → 卡死。修法：**GPU 转换只在渲染线程做**（`main.cpp` 调 `EnsureGpu/ConvertOnGpu`）。
7. **画面一闪一闪**：`main.cpp` 每帧把 `displaySrv` 重置成默认值 → 只有"收到新帧"那一帧对。修法：`g_displaySrv/g_displayW/g_displayH` **跨帧保持**，只有新帧到来才更新。
8. **MF 解码器只提供 NV12、不提供 ARGB32**（确认过 `outputFormat`）→ 必须自己转 BGRA，所以上 GPU 转换。

---

## 5. 当前未解决 —— 现象 / 根因 / 修复方案

### 5.1 帧数不满（核心问题）—— ✅ 已修复（2026-10-12，待实测确认）

**现象**：PC `recv` 每秒完成 **120~121 帧**，但 `decode` 只有 ~102fps（静止画面时甚至 25~30fps）。

**根因**：解码线程与收包线程之间用的是**"只留最新帧"交接槽**（`frameToDecode_` + `hasFrameToDecode_`）。WiFi 的 A-MPDU 聚合会让帧**成串到达**——一串里来 2~3 帧，解码只来得及拿 1 帧，其余被新帧覆盖**直接丢弃**。所以有效输入 < 120。`decode` 日志里的 `等待=2~4ms` 就是它被饿等的证据。
（解码本身只要 5.4ms，容量 ~185fps，**够 120fps**，不是瓶颈。）

**已实施修复**（`h264_udp_receiver.{h,cpp}`）：把"只留最新"改成 **FIFO 小队列（深 3）**：
- 收包线程把完整帧 `push_back` 进 `std::deque`；满 3 帧则丢**最老**（`pop_front`）并置 `needIdr_`（丢帧会打断参考链，故立刻请求关键帧）。
- 解码线程从队头 `swap` 取，旧缓冲进 `recycle_` 回收池复用 → **零逐帧 malloc**。
- 解码有 ~36% 空闲，队列平时基本是空的，**延迟几乎不增加**，就能吃满 120fps。
- 新增诊断：recv 日志加了 `dropped=` 计数，可看队列是否还在溢出。

**验证方法**：跑起来看 `castor_h264_udp.log` 里 `recv ... completed=` 与 `decode: X fps` 是否都≈120、且 `dropped=` 不再增长。

### 5.2 运动画面撕裂 / 拖影

**现象**：用户描述"运动的地方有很模糊的拖影"，且**不是垂直同步能解决的**（区别于经典撕裂）。

**已排除**：GPU 转换的绘制顺序（转换在前、ImGui 绘制在后，同一 immediate context 顺序执行）是对的。

**主假设（与 5.1 同一根因，已随 5.1 修复一起处理）**：**丢帧打断 H.264 参考链 → 时序伪影**。
- 之前"只留最新"会丢**整帧压缩数据**。若丢的是 **P 帧**，下一帧解码时参考的是解码器**从未见过的帧** → 该帧及其后续帧带着**错误/缺失参考**渲染 → 运动处出现**模糊拖影/残影**，直到下一个 IDR 恢复。
- 这与"静止时 decode 只有 25~30fps"（丢帧更多）也吻合。
- 5.1 的 FIFO 队列**按序喂帧**（正常不丢），参考链完整 → 拖影应随之消失；即便偶发溢出，也会立刻 `needIdr_` 请求关键帧复位。

**若 5.1 修复后拖影仍在，待查假设（按可能性）**：
1. **交换链**：`main.cpp` 用的是 `DXGI_SWAP_EFFECT_DISCARD` + `Present(0,0)`（legacy bitblt 路径）。可试 **Flip 模型 `DXGI_SWAP_EFFECT_FLIP_DISCARD`**（DX11 上用 `IDXGIFactory2::CreateSwapChainForHwnd` 重建交换链，`BufferCount=2`、`ResizeBuffers` 同步改）。
2. **显示端纹理读取与写入竞争**：确认 ImGui 采样 `gpuOut_` 的 SRV 时转换已完全结束（目前同线程顺序执行，理论 OK，可加 `Flush()` 验证）。

### 5.3 CPU 软件解码（用户关注点）

**现状**：**是 CPU 软解** —— `h264_decoder.cpp` 用 Windows Media Foundation 的 `CLSID_CMSH264DecoderMFT`（软件解码器），约 5.4ms/帧。

**关于"多线程"**：**外部多线程没用**。H.264 的帧是**时序依赖**的（P 帧依赖前一帧），解码序列天然串行；MFT 内部自己会按需多线程（宏块/WPP），我们无法从外面再并行。所以"开多线程解码"这条路不通。

**但它不是瓶颈**：5.4ms → 容量 ~185fps，**足够 120fps**。当前跑不满是 5.1 的交接丢帧问题。

**真想要余量 / 更低延迟**，唯一有效的是**硬件解码**：用 **D3D11VA**（把 DXGI device manager 接到 MFT 上，`MFT_MESSAGE_SET_D3D_MANAGER`），解码戳到 ~1~2ms，且输出直接是 GPU 纹理（连 NV12 上传都省了）。改动较大，**非必需**。

---

## 6. 关键文件与函数索引

**手机**（`Castor-Mobile/app/src/main/java/io/github/xiangsu1145/castor/`）：
- `MainActivity.kt` — `useUdp` 开关、通道按钮、`requestScreenCapture()/requestH264Probe()`、`serviceConnection` 里按通道分支。
- `stream/ScreenCaptureService.kt` — `startStreaming`(NDI) / `startUdpStream` / `startH264Probe`。
- `stream/H264UdpStreamer.kt` — 私有 UDP 推流：`setupCodecAndVirtualDisplay()`(编码配置)、`drainEncoder()`(分片发送)、`startBackchannelListener()`(PING/`!IDR`)。
- `stream/H264EncodeProbe.kt` — 纯实测探针（编码帧率/延迟/码率），按钮触发。
- `stream/ScreenBridge.kt` + `cpp/screenbridge.cpp` — NDI 用（`nativeNdiInit/Send/SendMeta`）。
- `net/LanDiscovery.kt` — 发现 PC IP + ClockSync。

**PC**（`Castor-Desktop/src/`）：
- `main.cpp` — 主循环；UDP/NDI/切片三路取帧；`g_displaySrv` 跨帧保持；`EnsureGpu/ConvertOnGpu` 在渲染线程调用。
- `h264_udp_receiver.{h,cpp}` — UDP 收包重组 + 解码线程(`DecodeLoop`) + GPU 转换(`EnsureGpu/ConvertOnGpu`，含 HLSL shader)。
- `h264_decoder.{h,cpp}` — MF H.264 软解 + `ConvertNV12ToBGRA_Fast`(SSE2，已改为 GPU 路径后仅作兜底)。
- `network_server.cpp` — 19889 信标应答。

**端口**：UDP **27190**(私有视频) / 19889(发现) / 5990(NDI 信标) / 27183(NDI)。

---

## 7. 怎么构建 / 怎么诊断

**构建**：
- PC：`Castor-Desktop\build.bat` → 产物 `Castor-Desktop\build\CastorDesktop.exe`（先 `taskkill` 旧的）。
- 手机：`Castor-Mobile\gradlew.bat assembleDebug` + `adb install -r -d -t app/build/outputs/apk/debug/app-debug.apk`。

**诊断**：
- **PC 端日志**：GUI 程序无控制台，接收器把诊断写到 **`Castor-Desktop\build\castor_h264_udp.log`**（每秒一行）：
  - `recv: pkts=.. completed=.. Mbps` —— 收包线程视角
  - `decode: X fps | 解=..ms 等待=..ms 发布=..ms` —— 解码线程视角
  - `输出类型: NV12 1088x1536 stride=..`、`GPU 转换就绪`、`frame seq=.. head=[..]`
- **手机端 logcat**：`adb logcat -s H264UdpStreamer H264Probe CastorLat`。

---

## 8. 下一步（建议顺序）

1. ~~**修 5.1**~~：**已实施**（`h264_udp_receiver.{h,cpp}` 改为深 3 FIFO + 缓冲回收 + 丢帧请求关键帧）。→ 待实测：`recv completed=` 与 `decode fps` 应都≈120，`dropped=` 不增长。
2. **复看 5.2**：帧率满之后再看撕裂/拖影是否还在（预期会随参考链修复一起消失）；仍在则试 Flip 交换链（见 5.2）。
3. （可选）**硬件解码 D3D11VA**：只为更低延迟/余量，非必需。
4. **叠 FEC**（协议层）：`H264UdpStreamer.drainEncoder` 加冗余包 + PC 端纠错，抗丢包。
5. 之后再做：**NDI/UDP 双模式开关打磨**、**键鼠映射通道**（独立小可靠通道）、ESP32-S3 对接。
