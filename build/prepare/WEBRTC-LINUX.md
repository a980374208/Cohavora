# Linux WebRTC v4 接入与硬编码适配评估

评估日期：2026-11-13。源码为 `a980374208/webrtc_hardEncoder` 的
`m_144_hard_encoder`，本轮固定最新提交
`b417823fe7faf6dc488c0161fc5b93a7d5d99810`。
构建参考 [v4 support 包](https://github.com/a980374208/Cohavora/releases/download/webrtc-m144-cohavora-bssl-dual-v4/cohavora-webrtc-build-support-m144-cohavora-bssl-dual-v4.zip)，
下载 ZIP 的 SHA-256 为
`0266d87877eb6bd0fdc6b2279194b989bd5efa7bae36aa840c64cc05979a683d`；
包内 82 个文件的 manifest 哈希校验通过。

## 本轮依赖替换范围

支持包的构建入口、CRT/STL 配置、静态库名和三个厂商后端面向 Windows，不能原样在
Linux 执行。Linux 构建使用固定 DEPS Clang/sysroot、C++20、libstdc++ ABI1、RTTI、
PIC 和普通 ELF archive；分别构建 Release、Debug，并根据实际 ELF C/C++ 与 GAS
符号重新生成各配置的 BoringSSL 前缀头。BoringSSL 头和宏继续隔离于系统 OpenSSL。

新 fork 未包含本项目现有的 external audio ownership 和 capture-mute 修复，故按原
顺序迁入两组补丁，保留新 fork 的源 options/APM 更新。另保留标准 ELF 重定位、
PipeWire 常量及系统头告警、许可证输出解析等 Linux 构建适配。

x265 的后端主体可跨平台；GN 增加 Linux 平台支持，第三方 x265 4.1 头的
`_MSC_VER` 未定义告警仅在 Linux Clang 的该 include 范围内屏蔽。
x265 采用固定源码、双配置静态 PIC、8-bit、NUMA 关闭；实际使用宿主 GCC，
不声称它与 WebRTC 共用 Clang/sysroot 或已验证更老的 glibc。

产品通过 SDK 自身的 `metadata/package.json` 选择 Linux profile，核对主库和
`libx265.a` 的指纹并传递 `RTC_ENABLE_H265`。稳定 v4 包不含 Linux 硬编；Linux 缺少
配套产品 H.265 解码器时，发送格式仍受解码器准入门限制。Windows 继续使用原
CRT/STL、库、宏及厂商源码路径；Linux 适配不增加 Windows 运行时分支。

产品当前 Room 的 codec 查询原先直接使用 `_stricmp`，导致 Linux 重建失败；现按
已有模式在编译期选择 Windows `_stricmp` 或 POSIX `strcasecmp`，比较及准入条件保持。
通用 `VideoFrame` 中的 D3D11 头和专属映射分支也限定于 Windows，Linux 沿用 NV12/I420
通用路径；Windows 分支的原语句保持。

稳定 v4 依赖替换的实际结果和独立验证范围见
[本地构建报告](../../samples/acceptance/reports/tooling/webrtc-linux-v4.html)。
软件编码、合成音频回归和构建结果不替代真实设备、服务、重连或完整产品验收。

## Linux 硬编候选的显式接入

后续候选 `cohavora-webrtc-linux-x64-b417823-hw-bounded-candidate-v5` 已实现
NVENC/CUDA、QSV/VAAPI DRM、隔离 FFmpeg VAAPI runtime；默认下载包与稳定 v4
保留。仅 Linux 显式打开产品开关时接入：

```bash
export WEBRTC_ROOT="/absolute/path/to/cohavora-webrtc-linux-x64-b417823-hw-bounded-candidate-v5"
cmake --preset ubuntu-product -B out/build/ubuntu-product-hw \
  -DQT_ROOT="$QT_ROOT" -DWEBRTC_ROOT="$WEBRTC_ROOT" \
  -DCOHAVORA_WEBRTC_PACKAGE_PROFILE="$WEBRTC_ROOT/metadata/package.json" \
  -DCOHAVORA_LINUX_HW_ENCODERS=ON
cmake --build out/build/ubuntu-product-hw --target cohavora_app --parallel 2
```

profile、公开头、Release／Debug 库、sender factory ABI 和 runtime 必须成组匹配。
CMake 核验合同及指纹；VAAPI runtime 复制到应用目录，安装包使用精确 `$ORIGIN`
RUNPATH，并附带对应 source／license／relink 材料。不要把 GPU 内核驱动打入包或设置
全局 `LD_LIBRARY_PATH`。Windows 保留原 D3D11 后端，不消费 Linux 宏或 runtime。

当前完成双配置 SDK 消费、有界故障注入、产品开关 ON／OFF 构建、软件本地
H.264／RTP／内容解码／释放和无 GPU 的 Qt 状态验证。三家 Linux GPU 实机仍未测试；
没有 `/dev/dri` 或 NVIDIA 设备的虚拟机不能因宿主有 AMD 显卡就取得硬编信用。
worker 不会终止挂起的 driver；timeout／RetirementFailed 后封存资源并拒绝自动回退，
不能宣称正常退役。实施、后续实机矩阵与边界见
[详细方案](LINUX-HARDWARE-ENCODING-PLAN.md)及
[候选测试记录](../../samples/acceptance/reports/tooling/linux-h264-first-slice.html)。

## OBS Linux 路径对照

参考 OBS 官方提交 `fe1ed7955f300efd17fc59f65938388e43d8bb06`。
当前 WebRTC fork 的 `VideoEncoderBackend::Frame` 已提供借用的 CPU NV12 平面，
调用在编码序列内串行，便于先实现 CPU 输入路径，并保持现有 ownership、码流检查、
改码率、关键帧和资源退役语义。

| 厂商 | OBS Linux 路径 | 当前 fork 的最小适配方向 |
| --- | --- | --- |
| Intel QSV | oneVPL + VAAPI DRM render node，支持 system-memory NV12 | 优先保留现有 sysmem NV12、EncodeFrameAsync/Sync 及校验；将 DXGI/D3D11 设备和 handle 替换为 render node、VA display、MFX_HANDLE_VA_DISPLAY，使用 POSIX 对齐内存。 |
| NVIDIA NVENC | CUDA context/array，CPU NV12 上传，也有 CUDA/OpenGL 互操作 | 新增 Linux 动态加载、CUDA context/设备、surface 上传和注册/释放；复用现有 codec 配置、码流约束和退役检查。首步不要求 Qt/OpenGL/DMA-BUF 接入。 |
| AMD | FFmpeg VAAPI H264/HEVC/AV1 | OBS 的 texture-AMF 仍仅 Windows；按 OBS 路径应新增 VAAPI 后端和明确的编码依赖。若坚持 AMF API，需另依据 AMD Linux/Vulkan 支持设计，不能将 OBS 当作已有 Linux AMF 实现。 |

具体参考：

- QSV：[构建依赖](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-qsv11/CMakeLists.txt#L12)、[Linux 设备和会话](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-qsv11/common_utils_linux.cpp#L269)、[system-memory 输入](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-qsv11/QSV_Encoder_Internal.cpp#L440)。
- NVENC：[加载 encode 库](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/nvenc-helpers.c#L102)、[加载 CUDA](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/cuda-helpers.c#L16)、[CUDA session](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/nvenc.c#L129)、[注册 surface 与 CPU 上传](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/nvenc-cuda.c#L134)。
- AMD：[平台源码选择](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-ffmpeg/CMakeLists.txt#L76)、[VAAPI 初始化](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-ffmpeg/obs-ffmpeg-vaapi.c#L151)、[CPU 上传](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-ffmpeg/obs-ffmpeg-vaapi.c#L711)。

候选采用独立 Linux 源码，由 GN 平台条件选择，保持 Windows 原源码和执行
路径。当前机器没有 `/dev/dri` 或 NVIDIA 设备/驱动，不能提供真实硬编码帧、动态
重建及设备释放的验收结论；对应工作应接入已有验收组及私有测试入口。
