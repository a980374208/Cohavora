# Linux 硬件编码实施方案（保持 Windows 兼容与性能路径）

编写日期：2026-11-13，America/Los_Angeles。本文中的“硬编码”指硬件视频编码。

状态：**Linux 三后端、产品/Qt 接线及候选 SDK 已实现；Linux 真实 GPU、跨机互通和产品性能验收未完成**。NVENC/CUDA、QSV/VAAPI DRM、隔离 VAAPI runtime 已进入独立 `hw-bounded-candidate-v5`，Release／Debug 构建、故障注入合同、包内消费和 runtime 重新链接验证通过。产品以 `COHAVORA_LINUX_HW_ENCODERS=ON/OFF` 分别完成 RelWithDebInfo 构建；实际产品工厂的软件 H.264 → 本地 RTP → 内容解码 → Release、无设备预检及 Wayland 设置状态验证通过。默认硬编开关仍为 OFF，原稳定 v4 SDK 与原产品输出保留。

当前 VMware 没有可直通的 GPU，Linux 三家真实硬编仍为 `NOT_RUN`。有界 worker 只限制调用方等待和保留资源数量，不能终止卡死的驱动；timeout／退役失败为进程级 sticky 故障，拒绝自动回退，显式 Software 路径独立可用。Windows GTX1050／UHD630 已完成官方 v4 SDK 的 CPU I420 本地闭环及观察基线，不能替代产品 native 输入配对性能或 Linux GPU 证据。最新已完成范围、工件身份及限制见 [候选实现与测试记录](../../samples/acceptance/reports/tooling/linux-h264-first-slice.html)；统一测试计划六个完整产品子门保持 `NOT_RUN`、完整产品信用 0。各阶段工时为初始估算，不是完成承诺。

## 1. 建议结论与交付边界

以 `/home/a/work/webrtc-cohavora-linux/v4/src` 的实际工作树为 SDK 基线，复用现有 `VideoEncoderBackend → BackendVideoEncoder` 的码流和 WebRTC 回调契约，新增 Linux 专属后端：

| 设备方向 | 建议主路径 | 第一阶段输入 | 产品首发编码 | 后续能力 |
| --- | --- | --- | --- | --- |
| Intel | oneVPL / QSV + VAAPI DRM display | CPU NV12，system-memory surface | H.264 Baseline、L1T1 | H.265、原生 surface 导入，分别准入 |
| NVIDIA | NVENC + CUDA | CPU NV12 → 复用的 CUDA surface | H.264 Baseline、L1T1 | H.265、CUDA/EGL/DMA-BUF 互操作，分别准入 |
| AMD | FFmpeg VAAPI，独立 Linux runtime 边界 | CPU NV12 → VAAPI hardware frame | 驱动实际支持的 H.264 Baseline、L1T1 | H.265；Main/High profile 为另一个协商扩展 |
| 其他设备 / 硬编不可用 | 现有软件编码与发布策略 | 原 CPU 输入 | 原有受支持 codec | 不按 GPU 厂商推断能力 |

VAAPI 是 backend/API，不是 AMD 专属能力；也可能支持 Intel。Intel 默认优先 QSV，在实际探测通过时才把同设备 VAAPI 作为候选。NVIDIA 不依赖第三方 VAAPI 桥接层作为默认支持路径。Linux AMF/Vulkan 不纳入首发主路径。

先完成 **CPU 输入的真实硬件编码闭环**，再考虑 DMA-BUF。第一阶段仍存在采集转换和 CPU→GPU 上传，不宣称端到端零拷贝。首发不自动开启 H.265 或硬件 AV1，也不把 OBS 支持的 codec 全部视为当前 SDK 已支持。

**Windows 是每阶段的兼容门，而非收尾工作。** Linux 的依赖加载、设备探测、worker、故障隔离、帧转换和扩展统计必须在编译期隔离。Windows 继续走已有 D3D11/NVENC/QSV/AMF 路径，不因 Linux 适配新增每帧分支、拷贝、锁、线程跳转或等待。

## 2. 当前代码基线与已确认缺口

### 2.1 基线身份

| 范围 | 本次核对到的身份 | 解释 |
| --- | --- | --- |
| Cohavora | `c0c054331905d88f8cd2a898a84a40558454e7d0`，工作树已有改动，staged 为空 | 当前 Linux v4 接入的未提交改动也是评估输入，不能只按 HEAD 重建 |
| 最新 SDK 源码 | `v4/src`，`b417823fe7faf6dc488c0161fc5b93a7d5d99810`，工作树已有改动 | 包括 Linux/x265 适配和既有音频修复；顶层目录不是 Git 仓库 |
| 旧 SDK 源码 | `src`，`aaeeee8077eb0a4cad1c9494e9c6433c751ef663` | 保留追溯，不作为本方案新实现基线 |
| 当前消费包 | `artifacts/cohavora-webrtc-linux-x64-b417823-cohavora-bssl-v4` | 产品 `ubuntu-product` 当前缓存指向此包，构建类型为 RelWithDebInfo |
| OBS 官方参考 | `fe1ed7955f300efd17fc59f65938388e43d8bb06` | 本次通过官方 GitHub API 核对 master，并阅读固定版本源码 |

实施开始时再记录 HEAD、工作树补丁 SHA、DEPS、GN 参数、工具链、头文件树与库 SHA；这些身份必须同时绑定，不能仅记录提交号。保留 v4 累计补丁中的 external audio ownership/capture-mute 修复，不重复叠加旧输入补丁。

### 2.2 实施前能力与缺口

1. `v4/configs/Release.gn` 已开启 H.265/x265；`rtc_use_nvenc/qsv/amf` 都是 false。三家 hardware backend 的 GN 仍要求 Windows，源文件依赖 D3D11、Windows loader 或 DXGI。
2. Linux 软件 H.264 是 OpenH264，不是 x264；VP8/VP9 为 libvpx，AV1 为 libaom。x265 成功编码是软件能力，不能标为硬件能力。
3. 当前 backend 只定义 H.264/H.265。输入是借用的 CPU NV12，`Encode()` 成功返回前完成当前帧；输出是归调用方所有的完整 Annex B access unit。没有现成 DMA-BUF backend 契约。
4. 公共适配器限制偶数尺寸、L1T1、单实例单流；H.264 校验 SPS `profile_idc=66`，实际 level 不得超过协商值，请求关键帧必须产出带 SPS/PPS 的 IDR。驱动只有 Main/High 时必须拒绝当前 Baseline 能力，不得改标签冒充。
5. SDK 内建 decoder factory 没有 H.265。产品现有 H.265 decoder runtime 依赖 Windows DLL loader，Linux 发送格式也受接收能力准入限制。宏 `RTC_ENABLE_H265` 不等于产品 H.265 双向可用。
6. 产品 `COHAVORA_HAS_BACKEND_ENCODERS` 当前关联 Windows D3D11 路径；Linux reviewed profile 只接通 x265。不能给 Linux 直接定义该宏解决硬编。
7. 产品偏好、能力 DTO、Room 预检、设置页、sender witness 和 telemetry 当前只认识 AMF/NVENC/QSV；增加 VAAPI 必须贯通这些层。
8. Linux 摄像头现在复制为 I420；PipeWire 当前接受 MemPtr/MemFd 后转换为 I420，没有 DMA-BUF 发送链。编码改造不能顺便把这些输入视为 native GPU frame。
9. 本机当前没有 `/dev/dri` 或 `/dev/nvidia0`。已有 v4 证据覆盖构建、ELF/ABI/符号、消费、小规模 x265 软件编码及合成音频，不覆盖真实硬编、实服 RTP、Windows 性能或长期稳定性。

以上为实施前基线，不能用于推断当前候选仍缺少三家后端。原接入说明见 [WEBRTC-LINUX.md](WEBRTC-LINUX.md)；以下设计要求仍约束尚未完成的实机验收。

### 2.3 当前实现与已验证边界

- 候选 SDK：`cohavora-webrtc-linux-x64-b417823-hw-bounded-candidate-v5`，profile SHA-256 `148803564bca786dc92320b43f7fe28c881b16c005a3fd83534506d835e2c441`；归档 SHA-256 `6664650e74d9c1fc2fd85f8816bbea5fecd4517fa42003eda46d58b284d5cf37`。两个配置各包含 3,443 个嵌入 ELF 对象；Debug 内嵌 DWARF，无外部 split-debug 依赖。
- Linux 后端均使用独立源码及 GN 开关。设备选择冻结 typed identity；有界 worker 持有输入和迟到任务的资源，超时后封存。每配置 bounded 23、NVENC 28、QSV 50、VAAPI 22 项 fake 合同检查通过，不授 GPU 信用。
- VAAPI runtime 使用窄 C ABI2，唯一导出 `cohavora_vaapi_get_api`。包内源码、许可证、生成头和 relink 材料已验证可逐字节复现 runtime；软件 H.264 实际编解码与显式加载 runtime 共存验证通过。
- 发布预检冻结 backend、设备、generation 及批准格式。仅 Linux 硬编构建启用 sender factory ABI，primary 在 offer 前安装不可变工厂，backup 固定 Software；失败走既有 rollback。真实 sender/channel/Call 生命周期与软件 RTP 闭环分别验证，不能合并成真实硬编发布通过。
- 能力缓存、偏好迁移、typed reason、发布状态和 Qt 设置页已贯通。无设备时四类预检正确拒绝，Software 不探测硬件；真实 Wayland 设置状态 34 项验证通过。物理摄像头、共享并发、LiveKit 实服重连与 GPU 释放仍需独立实测。
- 新 Release 库已完成现有双音轨隔离及重配置 extended 回归，并进入 Linux 系统音频指纹 allowlist；Debug 未取得同范围音频行为结果。不能继承为 PulseAudio 自动恢复通过。
- 独立诊断确认 S0 已存在 SDK／vcpkg Abseil 配置差异。本机所测公开对象布局相同，但三参数构造与 Protobuf Cord 符号负控证实依赖边界未统一。该风险尚未关闭；不得用全局前置 SDK Abseil 头作为局部修复。

## 3. OBS 参考方式与实时会议差异

| 主题 | OBS 固定源码可借鉴的行为 | Cohavora/WebRTC 必须另行实现的约束 |
| --- | --- | --- |
| QSV Linux | oneVPL 加载与硬件会话；DRM render node → VA display；system-memory NV12 | 正确选择编码设备；每次 Sync 有限预算；拥塞控制改码率；帧 ID 对应；不能照搬长等待和设备忙循环 |
| NVENC Linux | 加载 `libcuda.so.1` / `libnvidia-encode.so.1`；CUDA context、资源注册、平面上传与释放 | `NV_ENC_DEVICE_TYPE_CUDA`；关闭 B 帧/lookahead，按 RTC 校验低延迟；不得沿用 OBS 的多帧 output delay 默认值 |
| VAAPI | FFmpeg hardware device/frame context，NV12 上传和编码，DRM 节点选择 | 无重排序、明确关键帧/参数集、Annex B、动态码率/帧率、`EAGAIN` 有界处理；OBS 没有 `.update` 不能证明可实时改码率 |
| AMD | OBS Linux 使用 VAAPI；texture-AMF 源码在 Windows 条件下编译 | Linux AMD 不复用 D3D11 AMF 名称或代码，不以 OBS 为 Linux AMF 实现依据 |
| 平台隔离 | 平台独立源码及构建条件 | Linux 优化与开销仅进 Linux target；公共协议/状态改动做 Windows 回归 |

参考的是设备、资源和平台划分，不引入 OBS 的录制/muxer、UI、整套插件系统或直播默认参数。OBS GPL 源码用于行为研究；若需要移植代码段，先核对代码及第三方依赖许可证，保留所需归属信息，不能直接当作 WebRTC BSD 自有实现复制。

## 4. 模块划分与数据流

### 4.1 编码和状态的完整链路

```text
Linux 摄像头 / PipeWire
  → 现有 VideoSource（首阶段 I420 或 CPU NV12）
  → WebRTC 编码序列
  → Linux 工厂选择结果（backend + device + codec/profile + policy generation）
  → 现有 BackendVideoEncoder 契约
  → Linux 专属 backend 执行封装（owned NV12 pool + 有界 worker）
  → QSV Linux / NVENC CUDA / VAAPI Linux
  → 完整且独立持有的 encoded packet
  → OnEncodedImage → RTP sender → LiveKit → 对端解码

能力 / 首个实际硬编输出 / 回退 / 失败
  → 现有 native/business owner
  → Session Strand / SessionRuntime
  → immutable DTO / Qt queued signal
  → 设置与会议统计 UI
```

UI 不拥有 VA display、CUDA context、编码 session、buffer 或 native frame 的生命周期。编码实例在自己的编码/worker 序列持有资源；Session owner 管发布、取消、generation 和 UI 投影。回调不访问 QWidget，也不把驱动同步等待放到采集线程或 Session Strand。

### 4.2 最小改动边界

SDK 首选新增 `*_linux.cc` / Linux 专属 target，保留 Windows 同名厂商实现原文件与原 source list。通用 `BackendVideoEncoder` 及码流检查按原合同复用，不为 Linux 放宽全平台约束。Linux 所需设备注册表、VAAPI backend、超时/缓存放新模块；Windows 原三厂商 probe coordinator 不因 Linux 需求引入新的锁、枚举或缓存查询。

产品新增 Linux 工厂/能力桥接文件，由 `webrtc_manager.cpp` 在编译期选用。公共 DTO 可增加默认值明确的轻量字段，但 Linux driver detail 必须留在 Linux 实现中。SDK 包、产品头文件和静态库按新版本一起交付，禁止新头配旧库。

### 4.3 Linux 构建开关

| 层 | 开关/能力 | 默认与约束 |
| --- | --- | --- |
| SDK GN | `rtc_use_linux_nvenc` | 初期 false；只允许 Linux；独立选 CUDA 实现，不改 Windows `rtc_use_nvenc` 的默认值 |
| SDK GN | `rtc_use_linux_qsv` | 初期 false；只允许 Linux；显式固定 oneVPL、libva/libdrm 头和运行时基线 |
| SDK GN | `rtc_use_linux_vaapi` | 初期 false；只允许 Linux；由经过隔离核验的 VAAPI runtime 提供 |
| 产品 CMake | `COHAVORA_LINUX_HW_ENCODERS` | 初期 OFF；Linux 显式 opt-in 且包内能力匹配才开启 |
| 后续设计 | `COHAVORA_LINUX_DMABUF_ENCODING` | 当前未新增此开关；DMA-BUF 仍需独立设计与准入 |
| 产品内部宏 | `COHAVORA_HAS_LINUX_HW_ENCODERS` | 仅 Linux 目标设置，不能复用 Windows 宏 |
| 包 metadata | Linux backend、版本、编译能力与依赖身份 | 构建能力和运行时可用能力分开；runtime probe 失败不能改变 manifest |

GN/CMake 条件按目标平台判断，不按宿主平台猜测。不使用宽泛 `UNIX` 把 macOS 包含进来。Windows 错设 Linux 开关为 ON 时 configure 直接给明确错误；Linux OFF 时没有新设备探测、helper、worker 或 driver 库依赖。开关为 OFF 的配置须与当前软件基线保持可用。

## 5. 设备发现、能力和选择政策

### 5.1 发现与能力证明

按四级分别记录，不能折叠成一个 `supported=true`：

1. **Compiled**：包确实包含该 backend 和所需 API 版本。
2. **Discovered**：找到候选设备与驱动；设备具备访问权限，不代表支持编码。
3. **ProbeReady**：指定 codec/profile/尺寸/码率可 Open 并成功 Close。UI 可以显示“可用候选”。
4. **EncodingActive**：真实轨道产生至少一个经验证的硬件编码输出；RTP 与对端 decode/marker 是独立的交付证据。

Linux 用 DRM render nodes 与 CUDA 设备 API 枚举；不硬编码 `renderD128`、GPU 0 或“显示 GPU 必然是编码 GPU”。保留 backend、设备、驱动/API 版本、profile、level、最大尺寸、像素格式、实际 rate-control 能力；同一 GPU 的 QSV/VAAPI 是不同 backend，共用硬件 session 预算时不能重复算成两个设备。

默认在既有后台能力探测服务工作；只在启动、媒体设置需要或设备状态变化时执行。缓存键包含 device identity、driver/API version、codec/profile、能力参数及进程 generation，不在每帧枚举。设备移除、权限变化和不可恢复错误使缓存失效。设备完整路径/PCI/UUID 可进入受保护测试证据，UI/普通日志只输出稳定别名和枚举原因。

为防驱动 probe 阻塞/崩溃，Linux 可用短生命周期 helper 做实际 Open/Close 探测；固定 IPC 数据结构、超时、输出上限和 child ownership，调用方取消后拒绝旧结果。helper 只用于能力探测，不引入每帧进程 IPC。helper 成功后，实际实例仍必须验证设备身份和 Open 结果，不能把能力缓存当作资源预留。

### 5.2 选择与回退规则

当前 Auto 顺序为：实际可用 NVENC → QSV → VAAPI；同一 backend 内先选机器配置的设备，再按稳定设备序排序。此为初始确定性政策，非“某厂商性能更好”的结论；多 GPU 能耗/拷贝成本需实测后单独调整。预检与 Create 消费同一冻结选择计划，禁止预检 A 而运行 B。

| 请求 | 建议语义 |
| --- | --- |
| Auto | 初始化阶段可尝试已探测候选；运行中遵循既有发布/codec 回退合同，避免后台在多家硬件间反复切换 |
| 显式 NVENC/QSV/VAAPI | 厂商/API 候选隔离；不能静默换另一 backend；是否允许软件回退沿用明确的发布政策并投影真实结果 |
| Software | 不加载 Linux 硬件 driver，不做硬件预检 |
| 强制硬编测试 | 软件输出一律不算硬编 PASS；初始化不可用、回退和不支持分别留证 |

同 codec 的软件 fallback 必须保持协商 profile/level。H.264→VP8 等跨 codec 恢复只能经现有 encoder selector/Room 发布与协商政策，更新 SFU metadata 并以最终实际 codec 为准。未经对端/allowlist 允许不能偷偷换 codec。H.265 不得在同一 RTP payload 下换成 H.264。

`kRetirementFailed` 表示资源退役不明，必须封存相关 backend/device 并使当前事务失败；不能把它按普通 unavailable 继续软件发布或继续试另一家。普通缺库/无设备/不支持与驱动超时/退役失败是不同结果。未知 Windows 专属偏好在 Linux、Linux `vaapi` 偏好在 Windows 的持久化恢复，应规范化为 Auto 并给固定原因；不重写另一平台已保存的有效配置，平台专用设备选择独立存储。

## 6. 三条后端的具体实现

### 6.1 QSV Linux：oneVPL + VAAPI

拟增加 `qsv_video_encoder_backend_linux.cc`，复用既有 QSV 参数/码流约束，不把 D3D11 代码改成多层运行时分支。

1. 枚举可访问 DRM render node，`open(..., O_CLOEXEC)`，`vaGetDisplayDRM` / `vaInitialize`；由 RAII 按逆序终止 VA display、关闭 fd。无需依赖 X11/Wayland display。
2. oneVPL 硬件实现过滤使用 VAAPI acceleration mode；把已选 VA display 传入 `MFX_HANDLE_VA_DISPLAY`，核对最终 session 绑定的设备。不能让 dispatcher 默认选中另一张 GPU。
3. 第一阶段 system-memory NV12，POSIX 对齐分配和复用 surface；分开处理逻辑宽高、对齐宽高、pitch 与 crop，拒绝越界尺寸/stride。
4. 低延迟初始参数采用单帧流水深度、无 B 帧、无 lookahead；具体 async depth/rate-control 能力以 Query/实际输出核验。`DEVICE_BUSY`、`MORE_DATA` 等必须有有界状态处理，不能无期限重试。
5. `EncodeFrameAsync` 后在编码 owner 上等待本帧完成，输出保留 frame_id；不能照搬 OBS 的 60 秒同步等待预算。正常编码与 teardown 预算分开，超时进入明确失败流程。
6. `SetRates` 优先使用驱动支持的 Reset/更新路径；若需要重建，停接新任务、退役旧资源、按新配置创建、首帧 IDR。更新失败必须传播；不得悄悄继续用旧码率。
7. Query 通过后仍需真实 SPS/IDR、码率曲线、对端 decode、重复 Open/Close 和多实例证据。

### 6.2 NVENC Linux：CUDA 输入

拟增加 `nvenc_video_encoder_backend_linux.cc` 和 Linux loader/context 模块，Windows DirectX session 原样保留。

1. 运行时解析 `libcuda.so.1`、`libnvidia-encode.so.1`；校验所需 symbol、NVENC API 版本和 codec GUID。只有 NVML/nvidia-smi 成功不足以证明 NVENC 可用，也不要求引入完整 CUDA Toolkit runtime。
2. 按选中设备创建/持有 CUDA context，明确 push/pop/current context owner。禁止假定 Qt 渲染线程的 GL/CUDA context 就是编码 context。
3. 用 `NV_ENC_DEVICE_TYPE_CUDA` 打开 session。Linux 使用同步输出模型，不移植 Windows event/`enableEncodeAsync` 路径。固定 NV12 surface pool，初始化时注册资源；CPU 平面按 pitch 上传，避免每帧分配、注册和注销。若使用 pinned staging，容量有界并实测收益，不能照搬 OBS 每帧 host-register/unregister。CUDA array 或 device pointer 依 SDK 支持选择并实测，不同时承诺所有互操作方式。
4. 查询 capabilities 后配置低延迟、无 B 帧、无 lookahead、无时间重排序；关闭 OBS 录制用途的固定多帧输出延迟。输出映射必须验证每个 frame_id，不用“取最近帧”替代原始时间戳。
5. `NvEncReconfigureEncoder` 更新目标码率/帧率；强制 IDR 和输出 SPS/PPS 独立验证。平台不支持某更新时执行有界重建或返回失败，不丢失 WebRTC 拥塞控制信号。
6. Close 顺序覆盖解锁/取消映射、注销资源、销毁 bitstream/input/session、释放 CUDA 内存/context；驱动忙/退役失败时不能先释放仍被使用的 storage。
7. 缺库、旧驱动、无编码单元、session 配额不足、设备失效和多 GPU 均作为独立负向格子。

### 6.3 AMD / 通用 VAAPI：隔离的 FFmpeg backend

建议先采用 OBS 所验证的 FFmpeg VAAPI API 路线，但只提供 Linux 编码所需的窄适配，不把系统 FFmpeg 链入 Windows，也不把一套不受控 `libav*` 全局符号直接并入 WebRTC。

**先做依赖隔离小验证，再投入完整后端。** 当前 WebRTC 已静态包含 FFmpeg 相关对象，产品还有独立 H.265 runtime 设计。仅使用 `dlopen(..., RTLD_LOCAL)` 不能自动保证 ELF 符号不被主程序/其他 DSO 抢占。

建议构建 Linux 专属 VAAPI shared runtime：内部使用固定版本与构建参数的 FFmpeg/libva；外部只暴露窄 C ABI、版本、opaque handle、固定枚举和调用方所有的数据。公共 WebRTC/产品头不暴露 `AVFrame`/`AVCodecContext`。动态符号导出白名单、`readelf`/`nm`、实际绑定和同进程编码/解码测试共同验证隔离；若仍有冲突，采用私有符号前缀/隐藏实现的构建方案。不得以 `RTLD_DEEPBIND` 或全局 `LD_LIBRARY_PATH` 作为未经验证的修补。

固定 FFmpeg 配置仅包含必要 codec/VAAPI 能力，明确 LGPL/GPL 配置、动态依赖和分发材料；不为 VAAPI 自动引入另一份 x265/x264 或网络/muxer 功能。无法在选定 runtime 基线下可靠隔离时，暂停该路径，另行评估直接 libva backend；直接 libva 的 H.264 参数/码流生成和速率控制工作需重新估时，不能当作等量替换。

实现步骤：

1. 对已选 render node 建 VAAPI device/frame context；编码 hardware format 为 VAAPI，上传 software format 为 NV12，有限 surface pool。
2. 选择 `h264_vaapi`，查询真实 H.264 Baseline/Constrained Baseline 能力、尺寸/对齐/rate control。不能因为存在 encoder 名称就发布能力。
3. 关闭 B 帧和重排序，首发只准入能在同一次 `Encode()` 内完成当前输入帧的配置。send 返回 `EAGAIN` 时先读取已有输出并验证归属，不得把旧帧冒充当前帧；本帧已接受而 receive 返回 `EAGAIN`、需后续输入才能产出时，不准入当前同步适配器，判 unsupported/失败。libavcodec 的 `EAGAIN` 是状态机条件，时间流逝不能代替另一端 API 操作；驱动内部同步等待另设预算。不能忙循环 receive，也不能返回 `kNoOutput` 后在下一次调用交付旧帧。当前适配器要求成功 packet.frame_id 等于当前输入 ID。真正需要跨输入延迟输出时，另设计 Linux 异步适配器、完整帧元数据/时间戳映射和回调生命周期，单独验证后启用。
4. 输出统一为现有 Annex B access unit；处理 global header/extradata 与 SPS/PPS，强制关键帧必须证明是 IDR。禁止直接把 AVCC length-prefixed 数据交给当前适配器。
5. FFmpeg/驱动若不能就地改码率，`SetRates` 使用有界重建：更新请求合并为最新值、记录尚未应用的配置，退役成功后恢复并发送 IDR。若不能在预算内生效，传播失败/回退，不能静默忽略或无限频繁重建。
6. 测量重建频率、CPU、首帧、带宽收敛、设备 session 占用。该策略如不适合实际拥塞变化，VAAPI 不满足首发实时编码门，即使固定码率可以出帧也不能签发通过。

## 7. 帧 ownership、超时与恢复

第一版明确把 worker 放在 **`VideoEncoderBackend` 调用边界**：Linux 执行封装实现该接口并持有真实 backend，通过现有 `CreateBackendVideoEncoder` 注入。公共 `BackendVideoEncoder` 仍在调用侧管理 WebRTC 元数据和 `OnEncodedImage`，不把整个适配器搬到 worker 后继续持有 caller callback。

当前 `Frame` 只有借用指针；Linux 封装先把 NV12 两平面复制到可复用的 owned pool，再将 lease 交给 worker。Open/SetRates/Encode/Close 和真实 backend 析构在同一个 owner 上串行。任务返回独立 Packet，只有调用侧确认及时完成才发布；timeout 后 lease 留到 driver 调用和清理结束。不能保存 caller stack、`Packet*`、callback 指针或 QObject。首期这一 CPU staging copy 必须计入性能指标；后续仅在修改 Linux 契约能安全传入强引用后消除，不能假设现有 Frame 指针可直接零拷贝异步保存。

SDK 当前关闭 C++ exceptions；若执行封装落在 SDK，采用兼容该构建的状态/RAII 实现，不直接复制产品 `BoundedEncoderWorker` 的 throw/catch 实现或为此全局打开 exceptions。

采用小而有界的 in-flight 数量，第一版从一个工作任务开始；可等待的原始帧最多保留最新一帧，丢帧使用既有适配器语义并保留待发送关键帧请求。不能任意丢弃已编码参考帧或 packet。队列满、暂停、恢复、取消和错误计数分开。

Windows 不套这层新 worker，也不改其已有 BoundedEncoderWorker 的默认行为。Linux 的 Encode/Close 超时参数及 pool/queue 策略只进入 Linux 文件和配置。

重要边界：线程 wait timeout 不会中止 vendor driver 调用。超时后 seal worker，禁止重入；资源由仍执行的任务持有，迟到结果丢弃。只有任务返回并完成 Close 才能确认清理；退役尚未确认时按 retirement failure 封存，不能只返回普通 `kTimeout` 触发软件 fallback 并宣布成功。不能通过 detach、free 或取消线程伪造退出成功。原生 driver crash 仍可能影响进程；若产品要求硬隔离运行时 driver crash/hang，应另立“独立编码进程”阶段，包含 IPC/复制成本和设备恢复验证，不把 probe helper 当成实时编码隔离。

恢复必须关联 session generation、track revision、codec/profile、尺寸和设备身份。软重连可保留仍有效的实例；完整重连/重发布按现有事务规则重建或复用，恢复后的第一段媒体与新的 publication 对应。旧 generation 的能力结果、编码结果和 UI 更新一律拒绝。取消/离会与后台 probe 完成竞争时只产生一个终态。

## 8. 产品、Qt 与安全输出落点

| 位置 | 计划修改 | Windows 约束 |
| --- | --- | --- |
| `cmake/CohavoraWebRtcPackage.cmake` | 增加 Linux backend metadata/依赖与 ABI 指纹校验；当前只允许 x265 的规则按新 schema 精确扩展 | 原 Windows profile、CRT/STL、库/宏不变 |
| `CMakeLists.txt` | Linux 专用 source、runtime target、依赖与宏；Linux OFF 的构建对照 | Windows 不执行 Linux `find_package`、不产生 Linux target/link input |
| `src/rtc/linux/*` | 工厂、设备选择、probe、bounded wrapper、safe status bridge | 整个目录不进入 Windows 编译 |
| `src/rtc/webrtc_manager.{h,cpp}` | Linux 工厂入口、capability DTO 与 preflight；保留 H264 level guard/simulcast 和 H265 receive gate | Windows 原工厂顺序与创建语句维持原路径 |
| `src/core/room.cpp` | VAAPI 偏好、硬编准入、回退、最终 codec metadata；保留 generation/revision 二次检查 | 原 Windows 选择和发布政策不被 Linux 改写 |
| `src/net/session_manager.cpp` | backend 名称规范化、平台专属设备偏好、旧设置迁移 | Windows 不读取/启动 Linux runtime；无效跨平台值可解释地回 Auto |
| `src/ui/settings_dialog.cpp` | 根据 capability snapshot 显示候选，不按品牌硬编码能力；复用 AppTheme/既有控件 | Windows 现有有效选项、默认值保持 |
| `src/telemetry/codec_sender_witness.h` | 增加受控 Linux implementation identity，区分初始化与实际输出 | 保留原 Windows identity；不为识别 Linux 做逐帧字符串分配 |
| `src/telemetry/session_telemetry.cpp` 及既有 DTO 投影 | requested/effective codec/backend、fallback reason、probe/active 状态 | 复用原采样与聚合，不增加 Windows timer/统计频率 |
| `build/prepare` / Linux packaging | 新包身份、依赖、license、最低运行环境与回滚说明 | 不把 Linux DSO 或驱动放进 Windows 包 |

产品展示“自动 / 软件 / NVIDIA NVENC / Intel QSV / VAAPI”应取决于平台和能力；VAAPI 设备厂商可做说明。能力探测尚未完成、不可用、编码生效、回退软件、当前会话失败必须能区分。只有收到真实硬编输出后才能显示“硬件编码中”。

日志使用 typed reason，例如 `LibraryMissing`、`PermissionDenied`、`UnsupportedProfile`、`SessionLimit`、`DeviceLost`、`EncodeTimeout`、`RetirementFailed`。原始 driver/FFmpeg 错误只在业务内部用于判断，不通过改写错误字符串影响恢复语义；诊断导出不写 JWT、SDP/ICE、WebSocket URL、原始异常、用户路径或任意 driver 日志。

## 9. Windows 兼容与性能门

每个阶段都执行影响范围内的检查；不能等 Linux 功能完成后才评估 Windows。

| 层 | 必须满足的条件 | 证据 |
| --- | --- | --- |
| 构建图 | 无 libva/libdrm/oneVPL Linux/CUDA Linux loader/VAAPI runtime/DMABUF 源和库进入 Windows | GN/CMake source list、compile/link commands、依赖检查 |
| 头文件/ABI | 公共头可独立在 Windows 编译；新 SDK 头/库版本匹配；不混用 CRT/STL/Qt/WebRTC ABI | consumer compile/link、包 hash 与 ABI 宏 |
| 功能 | 原软件、NVENC、QSV、AMF、simulcast、profile/level、关键帧、回退、重连与 Release 维持 | 实际 backend × codec 格子；缺设备保持 NOT_RUN |
| 热路径 | 无新增逐帧 allocation、copy/readback、mutex、worker hop、driver probe、日志或 timer | 代码/构建审查 + 定点计数/采样，不能只看宏 |
| 性能 | 同机器同驱动同素材同参数，对照变更前后的 RelWithDebInfo | CPU、encode P50/P95/P99、首个输出、掉帧、内存、线程数、拷贝/上传计数 |

Windows 基线应从开始实施时的实际代码与相同 SDK 记录，不能拿不同源码或不同 Debug/Release 结果比较。至少三个交错配对窗口，预先固定负载、热身、窗口时长和仪器开销；用区间/噪声分析判断差异。出现可复现退化先停止放量，检查公共改动；没有足够样本标 INCONCLUSIVE，不用“在 Linux 构建通过”代替 Windows 性能结论。

已有 T013 的 OFF/ON 原门限仍按原定义使用；“Windows 改前/改后”属于新增适用场景，不把原遥测门限直接冒用为已冻结 Windows 硬编标准。新数值门限在 S0 固定到唯一测试计划后执行，不能测试后倒推阈值。

## 10. 分阶段实施与停止条件

阶段编号 S0–S6 为工程步骤，不是新增产品验收组。

| 阶段 | 主要工作与交付物 | 依赖 / 完成条件 | 估算 |
| --- | --- | --- | --- |
| S0 基线与准入 | 冻结代码/包；确认 Intel/NVIDIA/AMD 实机和驱动；保存 Windows 性能基线；VAAPI profile 与 FFmpeg 共存小验证；确定超时/性能阈值 | 不修改旧报告；不能隔离 FFmpeg 则先决策 VAAPI 路线；缺机器不宣布对应支持 | 2–4 人日 |
| S1 Linux 基础接线 | GN/CMake 开关、Linux factory/设备计划、typed capability、probe helper/缓存、manifest 与 OFF 对照；补 Qt 状态骨架 | Windows 构建图干净；无设备可启动且回软件；probe late result/取消测试通过 | 4–6 人日 |
| S2 QSV H264 | Linux oneVPL/VA display + sysmem；动态速率/IDR/Close；公开 factory 到实际 RTP | Intel 指定驱动上出帧/解码/释放；Windows QSV 不退化 | 4–7 人日 |
| S3 NVENC H264 | CUDA loader/context/surface；实时参数、重配、multi-GPU/session-limit | NVIDIA 实机完整闭环；不依赖桌面 GL；Windows NVENC 不退化 | 4–7 人日，可与 S2 并行 |
| S4 VAAPI H264 | 隔离 runtime、NV12 upload、Baseline/Annex B、动态更新/重建、AMD 实机 | S0 共存门通过；码率变化可用；profile 不满足时如实 unavailable | 6–10 人日，可在基础接口稳定后并行 |
| S5 产品与回归收口 | 三后端完整 UI/持久化/预检/实际 witness；camera+share、simulcast、网络恢复、资源清理、Windows 配对性能、安装包 | 各格独立结果；真实对端解码；无未解释性能/退役失败；按硬件支持清单放量 | 5–8 人日 |
| S6 后续优化 | H265 Linux 接收闭环；硬件 AV1 新契约；DMA-BUF / native surface；必要时运行时进程隔离 | 每项独立设计与准入，不作为 S2–S5 既成能力 | 单独估时 |

S0–S5 初始合计约 **25–42 人日**，不含等待硬件、复杂驱动缺陷、另写 direct-libva 或 S6。多人可并行做后端，基础契约、包 schema 和公共产品接口由一个 owner 收敛；估算不能替代实测发现后的调整。

首个实施切片建议：S0 + S1 + 一块 Intel 或 NVIDIA 实机上的 H.264 单流。以“真实编码输出 → RTP → 对端解码 → 正常释放 → Windows 对照”形成闭环后，再扩展剩余 backend。AMD-only 的目标设备优先完成 VAAPI 隔离/profile/动态码率前置，不能因为依赖路线更复杂而在计划中漏掉 AMD。

每条后端可独立以 feature OFF 回滚到现有软件路径。新 SDK 使用新 package ID 和独立输出目录，保留 v4 原包；回滚要同时恢复匹配头、库、runtime、profile，不仅换 `libwebrtc.a`。回滚依然服从已有会话生命周期；不能在活跃 session 中卸载仍被调用的 DSO。

## 11. 验证矩阵与证据要求

唯一计划入口为 [samples/acceptance/data/test-plan.json](../../samples/acceptance/data/test-plan.json)。本方案中的矩阵用于解释工程覆盖，实际待测子门与阈值只在该 JSON 维护。保留原 T001–T024 组和历史状态，不因添加 Linux 格子新增产品组或把旧组标 PASS。

### 11.1 最小矩阵

| 维度 | 必须覆盖或明确限制 |
| --- | --- |
| 后端/设备 | QSV Intel、NVENC NVIDIA、VAAPI AMD；补 Intel VAAPI；各声明驱动/设备代际分别记录，未拥有的设备 NOT_RUN |
| codec/profile | 首发 H264 Baseline；不支持 Main/High/H265/AV1 的请求正确拒绝；后续扩展各自重新准入 |
| 输入 | 确定性 I420/NV12；真实摄像头；Wayland screen/window；X11 screen/window 按产品声明覆盖 |
| 负载 | 360p/720p/1080p，30fps 为基线；60fps、1440p/4K 只对明确声明组合测试；奇数尺寸/超上限/异常 stride 拒绝或依原适配规则缩放 |
| 并发 | camera+share；H264 外层 simulcast 多实例；session 配额耗尽；反复开关共享；设备多 backend 并发 |
| 动态参数 | 降/升码率、framerate 更新、0 码率暂停/恢复、强制关键帧、分辨率变化、重发布 |
| 生命周期 | 预检取消、晚结果、快速入离会、软/完整重连、设备失效、Close 失败、清理超时；不得用进程退出代替正常资源释放 |
| 负向环境 | 无库、缺 symbol、版本过旧、无设备、节点无权限、无编码 profile、多 GPU 选错、driver busy / 迟迟无输出 |
| 互通 | Linux→Windows、Windows→Linux、Linux→独立支持的对端；以版本/codec/profile/marker 逐格记录，不拿同机 SDK 自回环代替跨平台 |
| Windows | 原硬件/软件选择、D3D11 native 输入、GPU scaling、simulcast、回退、退出；编译和物理性能两类证据 |

不用全维度机械笛卡尔积：每条硬件主链运行核心功能/生命周期；环境和动态组合按风险选择，但所有**对外声明的 backend × codec/profile × 设备/驱动**必须有正向实机证据。被省略的组合写清 unsupported/NOT_RUN，不能推算成通过。

### 11.2 每格的可复核证据

- 构建身份：产品与 SDK HEAD + dirty patch hash、preset/config、ELF/PE 与 runtime hash、驱动/API、OS/kernel、QPA；跨机时绑定双方版本。
- 选择证据：requested/effective codec、profile/level、backend 与匿名设备身份、策略/会话 generation、实际 `is_hardware_accelerated` 与 implementation witness。
- 媒体证据：输入帧、提交/完成帧、首个硬编输出、关键帧/SPS/PPS、packet/frame ID、RTP bytes/packets、对端 decoded frames 和 marker；发布/订阅成功本身不算送达。
- 速率和延迟：各时间点用单调时钟；排队、转换、上传、encode-submit→packet、首帧、码率收敛分别统计 P50/P95/P99/最大值及样本数。跨机端到端不得直接相减未校准墙钟。
- 资源证据：CPU、RSS、GPU memory/session、surface/FD 数、队列上限、丢帧、重建次数、退出后的清理结果；驱动无法提供的指标记 unavailable，不填 0。
- 性能对照：Linux 同设备同内容的软件 vs 硬件，并保持分辨率/fps/码率与质量目标可比；不能只比较编码时间而忽略上传/预处理 CPU，也不能把画质下降当性能收益。
- 正式状态：PASS_SCOPED / FAIL_SCOPED / NOT_RUN / INCONCLUSIVE 按实际范围；工具自测和 fake driver 不授硬件/产品通过。

新延迟、吞吐、恢复时限及资源增长阈值须在 S0 按目标设备冻结。吞吐需满足声明帧率且队列不持续增长；仅支持 30fps 的设备不能以 60fps 请求后静默降档通过。既有门限/授权例外继续保留。

### 11.3 复用私有工具与构建方式

先查现有私有导航，不另建日期/runXX wrapper：

- `Cohavora-private/tests/runtime/tools/webrtc_sdk/README.md`：已有 `webrtc-sdk consume`/符号与 package 工具；新增 backend API、真实编码、loader/fake-driver 测量能力时扩展该工具和 selftests。
- `Cohavora-private/tests/runtime/tools/linux_meeting/README.md`：已有 `linux-meeting pair/reconnect/peer`；扩展选择 backend/设备和 sender/receiver 证据参数。
- `Cohavora-private/tests/runtime/tools/linux_product/README.md`：已有完整 UI、screen share、preview 与实际 peer；扩展 Linux 编码状态/设备格子，复用同一执行入口。
- 所有可复用测试模块、配置模板和说明仍放上述私有目录；路径从忽略的机器配置解析。证据使用新目录；既有配置不得输出凭据。

调用形式保持 `python3 integration/run_private.py --config <机器配置> <现有工具> ...`。现有 `linux-meeting hardware-h264` 支持 `--backend nvenc|qsv|vaapi|software`、显式设备及 `--product-build-root`；`encoder-state-selftest --scope product` 连接实际产品工厂，`--scope all` 执行离线合同；`hardware-baseline` 采集 Windows 配对观察。参数与机器配置以对应私有 README 为准。fake driver、码流/参数单测、FD/owned-buffer/late completion 自测作为辅助合同检查，不另增产品验收项。

配置/依赖没变时复用已有 build tree；变化时才 configure。产品 Linux 使用 `ubuntu-product` / RelWithDebInfo，先构建受影响 SDK target、消费探针、`cohavora_core`，再构建 `cohavora_app`；需要双机 sample 时启用同 ABI 的 `simple_room`。Windows 使用匹配工具链的现有 preset，VS 多配置显式 `--config RelWithDebInfo`。首次执行 Windows MSBuild 按项目规则申请所需环境访问，不把 SDK 定位失败误记为源码错误。

报告默认更新原主题的 HTML/JSON；没有完成新测试不替换旧实测数据。新硬编主题确需独立报告时放 `samples/acceptance/reports/product/` 并登记目录，不能再造并行测试清单。被替代报告及 JSON 可追溯归档，原始证据保留；本次规划不生成冒充测试结果的报告。

## 12. 后续能力的独立准入

**H.265**：先完成 Linux 接收 decoder runtime、loader/ABI/符号隔离和产品双向协商；保留现有 SRST/L1T1、Main profile/tier/level、VPS/SPS/PPS/IDR 规则；确认 LiveKit 与目标对端版本支持，再分别启用三家编码。x265 回退仍属于软件，独立确认许可/分发范围。

**硬件 AV1**：当前 backend 的 `Codec` 与 Annex B NAL 检查只有 H264/H265，必须新增 AV1 OBU/sequence header、scalability、关键帧、RTP 与 factory 契约。OBS 有 AV1 encoder 和当前 libaom 软件可用都不等于这项工作完成。

**DMA-BUF / native frame**：仅 Linux 添加带引用 ownership 的 native buffer，明确定义 fd dup/close、fourcc/modifier、每平面 offset/stride、设备身份、fence 与 PipeWire buffer 归还时点。探测同 GPU 导入、跨 GPU、格式不支持与 fence 超时；失败可回到已经验证的 CPU 路径。`ToI420()` 兼容路径不能在常规 GPU 编码路径里意外触发 readback；所有 copy/readback 按路径留证。Qt/OpenGL 本地显示的资源 owner 与编码 owner 分离，不把预览纹理直接跨线程传递。

**严格故障隔离**：若需要在 driver crash 或不可中止的 hang 后继续可靠运行，另设计 Linux 独立编码进程和共享内存/FD 传输，明确崩溃恢复、资源预算、延迟和升级协议；此功能仍不进入 Windows 路径。

## 13. 源码依据与进一步定位

以下位置为本次读取时的行号；实施前以冻结的实际源码重新定位。

### 本地 SDK

- [`v4/configs/Release.gn:26`](/home/a/work/webrtc-cohavora-linux/v4/configs/Release.gn:26)：当前 Linux 功能开关。
- [`external/BUILD.gn:43`](/home/a/work/webrtc-cohavora-linux/v4/src/modules/video_coding/codecs/external/BUILD.gn:43)：Windows 硬件 backend 的平台条件；AMF/QSV 同文件。
- [`video_encoder_backend.h:18`](/home/a/work/webrtc-cohavora-linux/v4/src/modules/video_coding/codecs/external/video_encoder_backend.h:18)：codec/status/borrowed frame/同步合同。
- [`backend_video_encoder.cc:87`](/home/a/work/webrtc-cohavora-linux/v4/src/modules/video_coding/codecs/external/backend_video_encoder.cc:87)：单流/L1T1/尺寸；155 为速率状态，201 为输入转换，244 为 H264 码流检查。
- [`hardware_encoder_probe.h:19`](/home/a/work/webrtc-cohavora-linux/v4/src/modules/video_coding/codecs/external/hardware_encoder_probe.h:19)：原三厂商 coordinator 与 retirement poison。
- [`internal_encoder_factory.cc:89`](/home/a/work/webrtc-cohavora-linux/v4/src/media/engine/internal_encoder_factory.cc:89)：原厂商探针和 H264 advertisement。
- [`internal_decoder_factory.cc:47`](/home/a/work/webrtc-cohavora-linux/v4/src/media/engine/internal_decoder_factory.cc:47)：当前 decoder 集合。
- [`video_frame_buffer.h:34`](/home/a/work/webrtc-cohavora-linux/v4/src/api/video/video_frame_buffer.h:34)：native buffer 扩展点。
- [`v4/patches/README.md:3`](/home/a/work/webrtc-cohavora-linux/v4/patches/README.md:3)：累计补丁边界。

### 本地产品

- [`CohavoraWebRtcPackage.cmake:102`](/home/a/work/Cohavora/cmake/CohavoraWebRtcPackage.cmake:102)：Linux profile 目前只允许 x265 外部依赖；148 为 Windows backend 宏。
- [`webrtc_manager.cpp:41`](/home/a/work/Cohavora/src/rtc/webrtc_manager.cpp:41)：现有硬件宏下的 D3D11 依赖；1103 为 probe，1407 为 preflight。
- [`webrtc_manager.h:86`](/home/a/work/Cohavora/src/rtc/webrtc_manager.h:86)：能力 DTO。
- [`room.cpp:7093`](/home/a/work/Cohavora/src/core/room.cpp:7093)：预检与异步发布边界。
- [`bounded_encoder_worker.h:18`](/home/a/work/Cohavora/src/rtc/bounded_encoder_worker.h:18)：timeout 后保持资源 ownership 的现有模式。
- [`linux_camera_capture.cpp:153`](/home/a/work/Cohavora/src/media/linux_camera_capture.cpp:153)、[`pipewire_capture.cpp:356`](/home/a/work/Cohavora/src/media/linux/pipewire_capture.cpp:356)：当前 CPU 输入路径。
- [`codec_sender_witness.h:81`](/home/a/work/Cohavora/src/telemetry/codec_sender_witness.h:81)：实际编码实现 identity。
- [`CohavoraH265Decoder.cmake`](../../cmake/CohavoraH265Decoder.cmake)、[`h265_video_decoder.cpp:89`](/home/a/work/Cohavora/src/rtc/h265_video_decoder.cpp:89)：现有 Windows H265 runtime 边界。

### OBS 官方固定版本

- [参考 commit](https://github.com/obsproject/obs-studio/commit/fe1ed7955f300efd17fc59f65938388e43d8bb06)。
- [QSV 构建与平台依赖](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-qsv11/CMakeLists.txt#L12)、[Linux VA/DRM session](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-qsv11/common_utils_linux.cpp#L269)、[QSV surface/encode/sync](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-qsv11/QSV_Encoder_Internal.cpp#L440)。
- [NVENC driver loader](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/nvenc-helpers.c#L102)、[CUDA loader](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/cuda-helpers.c#L16)、[CUDA surfaces/upload](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/nvenc-cuda.c#L134)、[NVENC session/output delay](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/nvenc.c#L129)。
- [FFmpeg 平台源码选择](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-ffmpeg/CMakeLists.txt#L76)、[VAAPI device/frames/upload/encoder](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-ffmpeg/obs-ffmpeg-vaapi.c#L143)。

关键实时性差异的直接定位：[QSV 60秒 SyncOperation](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-qsv11/QSV_Encoder_Internal.cpp#L735)、[NVENC buffer/output delay](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-nvenc/nvenc.c#L301)、[VAAPI encoder 注册无 update 回调](https://github.com/obsproject/obs-studio/blob/fe1ed7955f300efd17fc59f65938388e43d8bb06/plugins/obs-ffmpeg/obs-ffmpeg-vaapi.c#L1206)。
