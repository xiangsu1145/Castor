# Castor Desktop - 极速串流与 AI 推理客户端

基于 **Direct3D 11 + Dear ImGui + Clang + CMake + Ninja** 打造的低延迟游戏投屏控制端。

---

## 🛠️ 技术特性

1. **图形与渲染管线**：
   - 原生 Direct3D 11 硬件加速渲染。
   - 采用 `DXGI_SWAP_EFFECT_DISCARD` / Flip 模型，消除呈现排队缓冲。
   - 内置中文字体支持（优先加载 Windows 系统微软雅黑 `msyh.ttc`，自动回退黑体/宋体）。
2. **UI 视觉与配色**：
   - 采用定制的 **Deep Cyber Slate（深海极客暗黑蓝）** 风格配色。
   - 优雅圆角、微光边框、流线型扁平组件。
   - 支持高 DPI 自适应缩放（Per-Monitor V2）。
3. **投屏控制台示例功能**：
   - **连接配置**：USB (ADB 本地映射极速通道) / Wi-Fi (局域网直连) 模式切换、自动重连、TCP_NODELAY 开关、单帧覆盖无缓冲模式。
   - **编码调优**：TurboJPEG (多核绑核切块)、H.264 (零缓冲硬编)、Raw 裸流预留；帧率 60/90/120/144 FPS、码率、质量、CPU 切块数与大核绑定。
   - **AI 推理预留**：全屏/中心 ROI 区域推理开关、ROI 占比调节、置信度阈值、NMS 阈值、DirectX-CUDA 显存零拷贝接口就绪状态。
   - **实时监控与性能 HUD**：平滑实时 FPS / 延迟曲线图，全链路耗时拆解（手机采集、编码、网络、PC解码、D3D11呈现）。
   - **右侧画面预览**：模拟游戏手机屏幕背板、准星与动态 AI ROI 检测框。

---

## 🚀 编译与运行

### 快速运行
编译好的可执行文件位于：
```cmd
D:\Castor\Castor-Desktop\build\CastorDesktop.exe
```
直接双击即可启动运行。

### 重新编译
工程已配置好 Clang-cl + CMake + Ninja 环境，运行 PowerShell 脚本：
```powershell
powershell -ExecutionPolicy Bypass -File D:\Castor\Castor-Desktop\build.ps1
```
