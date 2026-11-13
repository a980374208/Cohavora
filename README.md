# Cohavora

**基于 C++、Qt 和 WebRTC 的音视频会议客户端，对接 LiveKit 实时音视频服务。Windows 提供完整桌面客户端，Ubuntu 提供基础会议版。**

Cohavora 提供桌面会议界面，也提供可单独运行的命令行示例。你可以用它加入音视频房间、共享屏幕、聊天和使用协作白板，也可以从源码了解原生客户端的设备采集、会议状态管理和视频渲染实现。

当前支持 **Windows x64** 完整客户端和 **Ubuntu 24.04 x64** 基础会议版，使用 **C++20 + Qt 5.15.18**。账号登录、会议预约等业务功能需要兼容 OpenMeeting 的服务；只有 LiveKit 服务时，也可以通过地址和房间访问令牌直接连接。

- [开源依赖与致谢](#开源依赖与致谢)
- [主要功能](#主要功能)
- [快速开始](#快速开始)
- [加入第一场会议](#加入第一场会议)
- [开发构建](#开发构建)
- [代码结构](#代码结构)
- [打包应用](#打包应用)
- [常见问题](#常见问题)
- [许可证](#许可证)

## 开源依赖与致谢

感谢以下开源项目及其维护者。清单覆盖本仓库通过 CMake、vcpkg、Git 子模块和固定依赖包**明确引入或链接**的第三方库；同一个库即使通过多个渠道使用，也只列一次。

### 实时通信与通用组件

| 项目 | 在 Cohavora 中的用途 | 上游地址 |
| --- | --- | --- |
| WebRTC | 实时音视频和媒体传输；参考 LiveKit 构建方式自行编译并预打包 | [WebRTC](https://webrtc.googlesource.com/src)、[LiveKit 构建参考](https://github.com/livekit/rust-sdks)、[本项目包元数据](build/prepare/webrtc-package.json) |
| libyuv | 音视频帧格式转换，随 WebRTC SDK 提供 | [libyuv](https://chromium.googlesource.com/libyuv/libyuv) |
| BoringSSL | WebRTC SDK 随附的加密实现 | [BoringSSL](https://boringssl.googlesource.com/boringssl) |
| Asio | 异步网络连接与任务调度 | [Asio](https://github.com/chriskohlhoff/asio) |
| OpenSSL | 业务网络连接与部分桌面组件的 TLS/加密支持 | [OpenSSL](https://github.com/openssl/openssl) |
| Protocol Buffers | 信令和业务协议的代码生成与序列化 | [Protocol Buffers](https://github.com/protocolbuffers/protobuf) |
| spdlog | 日志基础设施 | [spdlog](https://github.com/gabime/spdlog) |
| zlib | 压缩；也被 Qt 桌面依赖使用 | [zlib](https://github.com/madler/zlib) |
| nlohmann/json | JSON 解析与生成 | [nlohmann/json](https://github.com/nlohmann/json) |

### 桌面 UI 与图形组件

| 项目 | 在 Cohavora 中的用途 | 上游地址 |
| --- | --- | --- |
| Qt 5.15.18 | 窗口、控件、网络、SVG 与翻译 | [Qt](https://github.com/qt/qt5) |
| Desktop App Toolkit lib_crl | UI 线程调度基础组件 | [lib_crl](https://github.com/desktop-app/lib_crl) |
| Desktop App Toolkit lib_rpl | 响应式事件处理 | [lib_rpl](https://github.com/desktop-app/lib_rpl) |
| Desktop App Toolkit lib_base | 桌面基础工具 | [lib_base](https://github.com/desktop-app/lib_base) |
| Desktop App Toolkit lib_ui | 桌面控件和样式 | [lib_ui](https://github.com/desktop-app/lib_ui) |
| Microsoft GSL | C++ Guidelines Support Library | [GSL](https://github.com/microsoft/GSL) |
| range-v3 | C++ 范围操作 | [range-v3](https://github.com/ericniebler/range-v3) |
| tl::expected | 结果与错误值处理 | [expected](https://github.com/TartanLlama/expected) |
| LZ4 | 桌面组件使用的压缩算法 | [LZ4](https://github.com/lz4/lz4) |
| xxHash | 桌面组件使用的快速哈希 | [xxHash](https://github.com/Cyan4973/xxHash) |
| tg_angle / ANGLE | Qt 桌面的 OpenGL ES/EGL 适配 | [tg_angle](https://github.com/desktop-app/tg_angle) |
| mozjpeg | Qt 图像插件使用的 JPEG 编解码库 | [mozjpeg](https://github.com/mozilla/mozjpeg) |
| libwebp | Qt 图像插件使用的 WebP 编解码库 | [libwebp](https://github.com/webmproject/libwebp) |

固定 Qt 静态包还明确链接了 [FreeType](https://freetype.org/)、[HarfBuzz](https://github.com/harfbuzz/harfbuzz)、[libpng](https://github.com/pnggroup/libpng) 和 [PCRE2](https://github.com/PCRE2Project/pcre2)，分别用于字体、文字排版、PNG 图像和正则表达式。仓库内随 Desktop App Toolkit 生成的样式代码，其原项目声明见 [Desktop App Toolkit 法律信息](https://github.com/desktop-app/legal)。

Qt、WebRTC 及其他依赖自身还可能包含传递依赖，实际分发时应以对应版本附带的许可证和声明为准。

## 主要功能

下表描述 Windows 完整客户端。Ubuntu 基础会议版提供 LiveKit 入会、远端音视频接收、本地麦克风与 V4L2 摄像头发布、设备选择及开关、重连提示和离会；账号、会议管理、聊天、屏幕共享及白板界面尚未接入。

| 功能 | 说明 |
| --- | --- |
| 音视频会议 | 麦克风静音、摄像头开关、参与者列表与视频显示 |
| 设备设置 | 选择摄像头、麦克风和扬声器，预览摄像头并测试音频设备 |
| 屏幕共享 | 共享屏幕或窗口，支持共享批注 |
| 会议聊天 | 发送文字、图片和文件，支持将本地图片或文件拖入聊天区或输入框，确认后发送 |
| 协作白板 | 绘图、文本、多页、图片导入及 PNG 导出，支持主持方控制编辑权限 |
| 会议管理 | 登录、按会议号入会、快速会议、预约、编辑和取消会议，需要配套业务服务 |
| 连接与诊断 | 重连状态提示与网格观看位置恢复、音视频统计和本地诊断报告 |
| 界面语言 | 默认简体中文，可通过启动参数切换英文 |

白板内容保留在当前会议窗口中，需要留存时请在离会前导出。通讯录等部分导航入口尚未实现。底层另有 RPC、数据流和端到端加密能力，其中端到端加密可通过 `simple_room` 示例体验，桌面端尚无用户密钥配置入口。

## 快速开始

Windows 可选择 [AI 分步引导](#方式一ai-辅助构建) 或 [手动编译](#方式二手动编译)；Ubuntu 参阅 [Ubuntu 从源码构建](#ubuntu-从源码构建基础会议版)。完成构建后，参阅[加入第一场会议](#加入第一场会议)配置服务连接。

### 方式一：AI 辅助构建

适合首次配置 Windows 构建环境，或需要协助诊断工具链与依赖问题的开发者。

1. 打开 <a href="build/prompts/windows-build-assistant.txt" target="_blank" rel="noopener noreferrer">Windows 环境部署与构建提示词（新页面）</a>，复制全文。
2. 将提示词交给具备本地文件与命令执行能力的 AI 助手，说明仓库路径或源码获取位置，并要求“按提示词协助配置环境并构建 Cohavora”。
3. 查看环境探测结果，在需要变更的阶段选择自动执行、手工处理或暂停。

助手会依据所选仓库的当前配置确定工具版本、依赖和命令，分阶段完成环境准备、兼容性诊断、构建与验证。

> GitHub 等平台可能不保留新窗口属性；可按住 Ctrl（macOS 为 ⌘）单击链接，或右键选择“在新标签页中打开”。

### 方式二：手动编译

以下命令使用仓库内置的 VS 2026 预设，在 **Visual Studio 2026 的 Developer PowerShell** 中执行。

#### 1. 准备工具

| 工具 | 要求 |
| --- | --- |
| 系统 | Windows x64 |
| Visual Studio | 2026，安装“使用 C++ 的桌面开发”工作负载及 Windows SDK |
| CMake | 推荐 4.3 或更新版本，以使用仓库内置的 VS 2026 预设 |
| Python | 3.8 或更新版本，确保 `python` 命令可用 |
| Git | 用于获取源码和 UI 子模块 |
| vcpkg | 已完成 bootstrap，用于安装 C++ 依赖 |

预设详情见 [CMakePresets.json](CMakePresets.json)；使用其他工具链时，需先确认生成器与预编译依赖的兼容性。

#### 2. 获取源码和 vcpkg

```powershell
git clone --recurse-submodules https://github.com/a980374208/Cohavora.git
cd Cohavora
```

如果已经克隆过仓库，在仓库根目录补齐子模块：

```powershell
git submodule update --init --recursive
```

如果尚未安装 vcpkg，可将它安装到独立目录：

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

在当前 PowerShell 会话中设置路径；已有 vcpkg 时换成自己的安装目录：

```powershell
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
```

CMake 会通过该路径加载 vcpkg，在加载 toolchain 前将 `VCPKG_MANIFEST_DIR` 默认设为 `build/vcpkg`，并根据 [build/vcpkg/vcpkg.json](build/vcpkg/vcpkg.json) 安装 Asio、Protobuf、spdlog 等依赖。默认使用 `x64-windows-static`。

#### 3. 准备依赖并编译

在仓库根目录执行：

```powershell
python build/prepare/prepare.py
cmake --preset windows-vs2026-release
cmake --build --preset release --parallel 2
```

第一条命令下载并校验固定版本的 WebRTC 和 Qt/Libraries，将它们放入 `deps/`。Qt/Libraries 压缩包约 **1.22 GB**，首次准备需要网络和足够的磁盘空间；后续运行会检查并复用有效缓存。首次 CMake 配置还会通过 vcpkg 安装依赖。

这里使用 Release 预设，只构建桌面应用及其依赖，不构建示例。`--parallel 2` 控制 MSBuild 的项目级并行度，可按机器配置调整；MSVC 还默认通过 `/MP` 并行编译同一项目中的源文件，因此编译进程总数可能高于 2，仍需留意内存占用。

#### 4. 启动应用

```powershell
.\out\build\windows-vs2026-release\src\app\Release\Cohavora.exe
```

使用 HTTP 和服务端交互时，需要携带 `--debug` 启动：

```powershell
.\out\build\windows-vs2026-release\src\app\Release\Cohavora.exe --debug
```

## 加入第一场会议

本仓库提供客户端。实际通话需要可访问的 LiveKit 服务；账号和会议管理还需要配套业务服务。Ubuntu 基础会议版启动后直接在初始窗口填写 LiveKit WebSocket 地址和有效房间令牌，下面的游客登录与账号操作步骤适用于 Windows 完整客户端。

### 方式一：使用 LiveKit 地址和令牌直连

适合已有 LiveKit 服务、想先验证音视频的开发者。

1. 准备 LiveKit WebSocket 地址，例如 `wss://your-project.livekit.cloud`，以及服务端签发的有效房间访问令牌（JWT）。令牌决定可以进入的房间、参与者身份和权限。
2. 启动应用，在登录页选择“游客访问”，填写昵称进入主页。
3. 点击“加入会议”，展开“高级 LiveKit 连接”。
4. 填写 LiveKit 地址和访问令牌后加入；直连模式可不填写会议号。

“游客访问”创建的是本地访客身份，连接 LiveKit 仍需要有效令牌。多人会议时，为各参与者签发属于同一房间、身份不同的令牌。

### 方式二：使用账号和会议号

适合已经部署兼容 OpenMeeting 业务服务的环境。

1. 在登录页打开“服务器设置”，填写 HTTPS 会议服务地址；需要注册时，按部署情况配置注册服务地址。
2. 使用账号登录，创建或预约会议，或者输入已有会议号加入。
3. 客户端向业务服务申请入会，获取对应的 LiveKit 地址和令牌，再建立音视频连接。

会议业务服务地址与 LiveKit WebSocket 地址用途不同，请分别填写。HTTP 业务服务仅在以 `--debug` 启动的开发模式下允许使用。

## 开发构建

以下操作沿用前面准备好的依赖和 `VCPKG_ROOT`。所有预设见 [CMakePresets.json](CMakePresets.json)。

配置辅助脚本位于 [build/configure.py](build/configure.py)，可在仓库根目录执行 `python build/configure.py --help` 查看选项；根目录的 `configure.bat` 会转发到该脚本。

### Ubuntu 从源码构建基础会议版

以下命令适用于 Ubuntu 24.04 x86_64。先安装编译工具、vcpkg 所需的下载工具，以及静态 Qt 运行时链接使用的系统开发包：

```bash
sudo apt-get update
sudo apt-get install --no-install-recommends \
  build-essential git python3 perl cmake ninja-build pkg-config \
  ca-certificates curl zip unzip tar autoconf automake libtool nasm \
  libssl-dev libfreetype-dev libfontconfig-dev libdbus-1-dev \
  libglib2.0-dev \
  libgl1-mesa-dev libegl1-mesa-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxrender-dev libxi-dev libsm-dev libice-dev \
  libxcb1-dev libxcb-glx0-dev libxcb-icccm4-dev libxcb-image0-dev \
  libxcb-keysyms1-dev libxcb-randr0-dev libxcb-render-util0-dev \
  libxcb-render0-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev \
  libxcb-xfixes0-dev libxcb-xinerama0-dev libxcb-xinput-dev libxcb-xkb-dev \
  libxkbcommon-dev libxkbcommon-x11-dev
```

获取源码。Ubuntu 基础会议版不依赖 Git 子模块：

```bash
git clone https://github.com/a980374208/Cohavora.git
cd Cohavora
```

已有克隆时，直接在仓库根目录继续。Ubuntu 构建无需运行 `git submodule update --init --recursive`；该命令还会尝试访问私有 SSH 子模块 `samples/acceptance`，公开账号可能因此失败。Windows 完整客户端另需其 UI 子模块，私有验收子模块不属于 Ubuntu 构建范围。

尚未安装 vcpkg 时，可在仓库的同级目录安装；已有 vcpkg 则将 `VCPKG_ROOT` 改为其实际目录：

```bash
git clone https://github.com/microsoft/vcpkg.git ../vcpkg
../vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT="$(realpath ../vcpkg)"
```

在仓库根目录准备固定版本依赖、配置并编译：

```bash
python3 build/prepare/prepare.py
cmake --preset ubuntu-ninja-relwithdebinfo
cmake --build --preset ubuntu-meeting --parallel 2
./out/build/ubuntu/src/app/Cohavora
```

准备脚本从项目 Release 下载并校验 Ubuntu 版静态 Qt 5.15.18 和 WebRTC/BoringSSL 双配置 SDK，默认分别安装到 `deps/Libraries/linux64` 和 `deps/webrtc`，下载归档缓存在 `deps/.archives/`；有效安装会在后续运行时复用。Qt ZIP 约 1.04 GB，WebRTC 压缩包约 243 MB，首次执行需要网络和数 GB 磁盘空间。网络受限时，可用 `--qt-archive`、`--webrtc-archive` 指定已下载的固定归档。

CMake 通过 `VCPKG_ROOT` 加载 vcpkg，以 `x64-linux` 和 [build/vcpkg/vcpkg.json](build/vcpkg/vcpkg.json) 安装 Asio、Protobuf、spdlog 等通用 C++ 依赖；默认安装目录为 `deps/vcpkg_installed`，下载缓存为 `deps/vcpkg_downloads`。

`ubuntu-meeting` 使用 `RelWithDebInfo`，只构建基础会议版及其依赖；旧名称 `ubuntu-receiver` 仍可使用。界面支持 LiveKit 入会、接收远端音视频、发布本地麦克风和 V4L2 摄像头、切换麦克风/扬声器/摄像头、重连提示及离会。摄像头需要可用的 `/dev/video*` 设备；账号登录和完整会议业务界面尚未接入 Linux。Ubuntu 与 Windows 的 Qt/WebRTC 二进制不可混用。

### Ubuntu 完整 UI 开发构建（P0–P6）

`ubuntu-product` 是 Linux 共用完整桌面 UI 的独立开发预设，使用 `RelWithDebInfo`，输出到 `out/build/ubuntu-product`。上面的 `ubuntu-meeting` 基础会议版仍保留；本预设不代表完整 Linux 产品已通过验收。

先完成上一节的工具、vcpkg 和基础开发包准备，再补齐完整 UI 所需的公开子模块及 Linux 开发包。只初始化 UI 路径，不需要私有验收子模块：

```bash
git submodule update --init --recursive -- src/ui/tdesktop
sudo apt-get install --no-install-recommends \
  libwayland-dev libwayland-bin libffi-dev \
  libglib2.0-dev libglib2.0-dev-bin libpcre2-dev \
  libmount-dev libblkid-dev libselinux1-dev libsepol-dev \
  libxtst-dev libgbm-dev libdrm-dev libpipewire-0.3-dev libspa-0.2-dev libpulse-dev \
  libsecret-1-dev
```

完整 UI 需要与本项目 Qt 5.15.18 补丁、编译器及 C++ ABI 匹配的 **静态 QtWayland SDK**，包括 `Qt5WaylandClient`、Wayland QPA、xdg-shell、窗口装饰及 EGL client integration 静态插件。QtWayland 源码应采用 Qt `v5.15.18-lts-lgpl` superproject 锁定的提交 `5a85cd79f9e16f9191d7187aa4b9f34b3f2c969b`；不能将发行版动态 QtWayland 插件混入本项目静态 Qt。需要 Linux 屏幕阅读器或 AT-SPI 桌面验收时，QtBase 还须启用 `accessibility-atspi-bridge`，并以该配置构建 QtWayland；仅启用通用 `accessibility` 不会提供 AT-SPI 桥。

**当前 `prepare.py` 下载的已发布 Ubuntu Qt 包尚不包含 QtWayland。** 准备脚本仍用于取得固定 WebRTC 和基础 Qt；请另外提供已经构建并验证的同 ABI QtWayland SDK，通过 `QT_ROOT` 明确选择。不要用旧包覆盖扩展 SDK。

```bash
python3 build/prepare/prepare.py
export QT_ROOT="/absolute/path/to/qt-5.15.18-with-wayland"
cmake --preset ubuntu-product -DQT_ROOT="$QT_ROOT"
cmake --build --preset ubuntu-product --parallel 2
./out/build/ubuntu-product/src/app/Cohavora
```

在原生 Wayland 桌面可显式使用 `QT_QPA_PLATFORM=wayland` 启动；X11 使用 `QT_QPA_PLATFORM=xcb`，两种运行结果分别验收。账号和会议业务仍需要兼容 OpenMeeting 的服务，真实媒体仍需要 LiveKit 连接配置；不要将凭据写进构建脚本或 CMake 缓存。

若已有构建目录后改用另一套 Qt SDK，重新配置时追加 `-U 'Qt5*_DIR' -DQT_ROOT="$QT_ROOT"`，清除旧 Qt 组件的 CMake 定位缓存；随后重建，避免混用不同安装目录的模块。

P3 系统音频采用 PulseAudio 播放流 monitor，兼容 pipewire-pulse，输出为独立的 `ScreenShareAudio` 轨道。共享默认关闭；共享设置可选择声音输出，屏幕或窗口的选择不限制声音范围。同一输出内，具有可识别进程身份的其他应用声音参与混音，本进程的播放音和无法识别来源的流被排除。最多同时混合 32 路；设备移除、服务断连或捕获失败会关闭共享音频，视频共享可继续。共享音频静音、停止与麦克风分开控制；可通过共享菜单选择输出并重新开启音频，无需重新选择共享画面。

已发布的 Linux WebRTC SDK 存在跨音轨静音和外部音源重新配置缺陷，因此 `COHAVORA_LINUX_SYSTEM_AUDIO_VERIFIED` 默认关闭，普通下载构建保留视频共享。本机 P3 audio-v2、m144 v4 及硬编候选 v5 的 Release SDK 已完成双轨隔离、编码器重建及音源切换回归；启用该选项时 CMake 仅接受已测 Release 库的指纹，不能用旧 SDK 强行开启。v4 Release SHA-256 为 `f4c5ebfbf50a609b8f4f3e22e246b911cec9eecea66606a27a94836f0f25e202`，候选 v5 为 `35c7a45caf749d1825ee3b4019e6a2ab1afb4d16fe5b96bb029b7a230280d8a2`；Debug SDK 尚未取得相同音频行为验证。使用匹配的完整 v4 SDK 根目录和 Linux profile 配置：

```bash
export WEBRTC_ROOT="/absolute/path/to/cohavora-webrtc-linux-x64-b417823-cohavora-bssl-v4"
cmake --preset ubuntu-product -DQT_ROOT="$QT_ROOT" \
  -DWEBRTC_ROOT="$WEBRTC_ROOT" \
  -DCOHAVORA_WEBRTC_PACKAGE_PROFILE="$WEBRTC_ROOT/metadata/package.json" \
  -DCOHAVORA_LINUX_SYSTEM_AUDIO_VERIFIED=ON
cmake --build --preset ubuntu-product --parallel 2
```

此 SDK 尚未替换准备脚本的公开下载包。构建或合成分轨回归不代表物理音质、自动恢复或完整会议验收通过；已记录的 PulseAudio 重初始化缺陷仍独立阻断完整音频重连验收。

v4 的 Linux/Windows 构建边界及 OBS 硬编适配见 [Linux WebRTC 接入说明](build/prepare/WEBRTC-LINUX.md)。独立候选 v5 已实现 Linux NVENC/CUDA、QSV/VAAPI DRM、VAAPI 后端及产品设置接线，产品开关 `COHAVORA_LINUX_HW_ENCODERS` 默认 OFF。候选双配置、产品 ON/OFF 构建、软件 RTP 闭环及无设备状态验证已通过；Linux 真实 GPU、跨机互通和性能仍未验收，不能作为默认发行支持声明。

完整 UI 同时使用 Qt 和 WebRTC 各自的 JPEG ABI。构建会用 Python 3、`nm` 和 `objcopy` 在构建目录生成 WebRTC 副本，隔离 Chromium JPEG 函数前缀遗漏的两个内部数据表，并记录原件与副本指纹；依赖目录的 SDK 保持原样。若更换依赖后出现额外符号冲突，构建会停止，需重新审查 ABI，不能忽略重复符号错误。

本机开发曾通过下载并解包 Ubuntu 开发包到用户级 sysroot 来验证依赖；这只是无 sudo 环境的构建折衷。普通 Ubuntu 开发机应优先使用上面的 apt 开发包。使用自备 sysroot 时，通过 `CMAKE_PREFIX_PATH` 和 `PKG_CONFIG_PATH` 指向其开发文件，不复制本机绝对路径，也不要把开发 sysroot 当作应用运行包分发。

P2 增加 Wayland 屏幕／窗口视频共享：在会议内选择共享类型、分辨率和帧率，再由系统选择器选择实际来源并授权；等待授权期间可以取消。桌面需要 ScreenCast Portal 及对应桌面后端、PipeWire 1.0 或更新版本。本实现仅使用 CPU 缓冲，复用原有发布与预览链路，不持久化授权；纯 X11 会话的捕获尚未接入。实现与 OBS 行为参考说明见 [Wayland capture](src/media/linux/README.md)。

原生 Wayland 可以发起旧 v2 协议的远程控制请求，物理键使用严格的 Set1/E0 映射；未知键拒绝。X11 控制端的物理键位映射尚未验证。Linux 被控方向因本地输入优先、可靠释放、屏幕绑定及后台紧急停止的安全能力不足而明确禁用；不通过 root、uinput 或私有桌面接口绕过授权。Windows 真机互通和文本／IME 仍须独立验证。

登录状态可异步保存到 Secret Service（GNOME Keyring），密码不持久化。缺少或拒绝密钥环服务时允许本次内存登录，并显示保存失败状态，不回退到明文文件。独立的 0600 元数据文件只保存条目 UUID、绑定及撤销状态。旧凭据来源保持禁用；无法确认删除时明确提示清理未完成，不把旧条目当作已删除。

当前实测 Ubuntu 24.04 的 `gnome-keyring 46.1-2ubuntu0.2` 在应用取消解锁提示时会崩溃；官方未修改 51.1 另有应用关窗后提示残留的问题。本地补丁候选已通过独立验证，但尚未作为 Ubuntu 发行依赖交付。开发预览可不勾选“记住登录”，使用本次内存会话；完整持久化验收仍开放。应用安装包不捆绑或替换系统密钥环服务，详见 [Ubuntu 打包说明](packaging/linux/README.md)。

本地 DEB、tar.gz 和调试信息包的构建入口见 [Ubuntu 打包说明](packaging/linux/README.md)。Linux 使用独立的签名更新清单与信任配置，默认没有已配置的 Linux 更新源；不会请求 Windows 安装器。可选更新通道支持验签、下载及保存 DEB，安装由退出会议和应用后的系统包管理器执行；正式发行优先使用签名 APT 仓库，详见 [Linux 更新说明](packaging/updates/README.md)。

Linux 共享批注、被控输入和音频设备测试仍禁用；系统音频受上述 SDK 验证开关限制，Linux E2EE Required 继续拒绝。完整 UI 启动、账号登录、真实设备、重连、安装升级和资源释放须分别取得同一构建的证据；基础版已有结果、离线 SDK smoke 以及 Windows 验收不能替代这些子门。

### 调试桌面应用

```powershell
cmake --preset windows-vs2026-dev
cmake --build --preset dev-debug --parallel 2
.\out\build\windows-vs2026-dev\src\app\Debug\Cohavora.exe
```

`dev-debug` 只构建应用及其依赖。项目的 Debug 配置保留应用调试符号，但 Qt 和默认 WebRTC 依赖仍使用 Release ABI，运行库统一为 `/MT`；不要随意混入 `/MD` 或不同 ABI 的库。

| 需求 | 配置预设 | 构建入口 |
| --- | --- | --- |
| 开发桌面界面 | `windows-vs2026-dev` | `dev-debug` 构建应用；`all-debug` 构建全部默认目标 |
| Release 应用 | `windows-vs2026-release` | 构建使用 `release` |

### 运行命令行示例

dev 配置提供三个示例目标：

| 目标 | 用途 |
| --- | --- |
| `simple_room` | 连接房间、发布示例音视频、观察参与者事件，可配置端到端加密 |
| `simple_rpc` | 演示参与者之间的 RPC 调用 |
| `cohavora_media_broadcaster` | 仅 Windows：通过 WASAPI / DirectShow 采集并发布麦克风、系统声音或摄像头媒体 |

Ubuntu 启用 `COHAVORA_BUILD_SAMPLES=ON` 时提供 `simple_room` 和 `simple_rpc`，不生成 `cohavora_media_broadcaster`。构建桌面应用请选择 `cohavora_app` 目标。

例如先查看 `simple_room` 的用法：

```powershell
cmake --build out/build/windows-vs2026-dev --config Debug --target simple_room --parallel 2
.\out\build\windows-vs2026-dev\samples\Debug\simple_room.exe --help
```

示例支持 `--url`、`--token` 参数，也支持 `LIVEKIT_URL`、`LIVEKIT_TOKEN` 环境变量。`simple_room` 和 `simple_rpc` 未提供连接参数时仅显示帮助，不会验证实际连接。访问令牌属于凭据，请勿写入源码或提交到仓库。

`simple_room` 默认要求安全信令传输。仅对明确使用 `ws` 的私有开发服务，可在该次运行环境中设置 `COHAVORA_SAMPLE_ALLOW_INSECURE_TRANSPORT=1`；默认不会根据 URL 自动放宽策略，此开关不改变桌面产品的传输设置。

## 代码结构

| 路径 | 主要职责 |
| --- | --- |
| `src/app/` | 桌面应用入口与基础示例入口 |
| `src/core/` | 房间、参与者、音视频轨道、会议协调和会话生命周期 |
| `src/core/whiteboard/` | 白板状态与协作逻辑 |
| `src/net/` | 会议业务 HTTP 接口、登录会话和凭据存储 |
| `src/signal/` | LiveKit 信令连接、消息与重连相关逻辑 |
| `src/rtc/`、`src/media/` | WebRTC 媒体处理与 Windows 设备采集 |
| `src/render/` | 视频帧路由及渲染后端 |
| `src/ui/` | Qt 界面、主题、翻译和白板控件 |
| `src/ui/tdesktop/` | 通过子模块引入的 UI 基础组件 |
| `src/e2ee/`、`src/rpc/` | 端到端加密、RPC 和消息类型 |
| `src/telemetry/` | 性能统计、诊断、日志脱敏和报告 |
| `protos/` | 信令与业务协议定义，C++ 文件在构建时生成 |
| `samples/` | 命令行示例构建定义 |
| `cmake/` | CMake 构建模块 |
| `build/configure.py`、`build/vcpkg/` | 配置辅助脚本与 vcpkg manifest |
| `build/prepare/` | 依赖准备脚本与固定包元数据 |

阅读源码时可以从 [桌面入口](src/app/main_meeting_app.cpp) 开始，沿着 [会议协调器](src/core/meeting_coordinator.cpp) 阅读到 [Room 实现](src/core/room.cpp)。

会话状态更新的大致路径如下。`Session Strand` 用于让会话状态按顺序处理，界面通过数据快照和 Qt 排队信号接收结果：

```mermaid
flowchart LR
    A[网络与 WebRTC 回调] --> B[Session Strand 串行处理]
    B --> C[SessionRuntime 会话状态]
    C --> D[数据快照与 Qt 排队信号]
    D --> E[Qt 界面]
```

音视频资源的生命周期由底层会话管理；修改会议状态或回调处理时，需要同时考虑离会、重连和旧回调晚到的情况。

## 打包应用

打包前先完成[快速开始](#快速开始)中的 Release 构建。打包命令只封装已有产物，**不会重新编译应用**；源码变更后应先重新执行 `cmake --build --preset release --parallel 2`。

### ZIP 便携包

```powershell
cpack --config out/build/windows-vs2026-release/CPackConfig.cmake -C Release -G ZIP
```

输出位于 `out/build/windows-vs2026-release/packages/`，文件名为 `Cohavora-<版本>-Windows-x64.zip`。包内运行文件的相对布局如下，解压和分发时须完整保留：

```text
Cohavora.exe
renderers/
  cohavora-render-dx11.dll
  cohavora-render-opengl.dll
```

### Inno Setup 安装包

安装 [Inno Setup](https://jrsoftware.org/isinfo.php) 后，用 [Windows 安装脚本](packaging/windows/CohavoraInstaller.iss) 编译安装包。建议使用 Inno Setup 7（已通过 7.1 编译验证）；使用 6.x 时至少需要 6.3，并需自行准备与编译器版本匹配的 `ChineseSimplified.isl`。英文使用编译器自带的 `Default.isl`。

安装包及卸载程序要求 Authenticode 签名，当前先使用本机自签名代码签名证书。通过 [打包入口](packaging/windows/build-installer.ps1) 执行时，会创建并复用当前用户证书库中的 `Cohavora Self-Signed` 证书，RSA 私钥标记为不可导出；公钥证书与指纹配置保存在 `%LOCALAPPDATA%\Cohavora\signing\authenticode-selfsigned.cer` 和 `authenticode-selfsigned.json`。配置损坏、证书缺失或过期时明确报错，不自动轮换证书。

在仓库根目录执行，按本机安装位置填写 ISCC 和 Windows SDK SignTool 路径；打包入口必须显式传入 `AppBuildDir` 和 `OutputDir`。下面使用标准 Release 预设的运行目录，输出目录须为新目录或空目录，避免复用其他证书的卸载程序签名缓存：

```powershell
$AppBuildDir = (Resolve-Path 'out/build/windows-vs2026-release/src/app/Release').Path
& .\packaging\windows\build-installer.ps1 `
    -Iscc 'C:\Program Files (x86)\Inno Setup 7\ISCC.exe' `
    -SignTool 'C:\Program Files (x86)\Windows Kits\10\bin\<SDK版本>\x64\signtool.exe' `
    -AppBuildDir $AppBuildDir `
    -OutputDir 'C:\packages\selfsigned-output'
```

该入口不会把证书加入受信任根。自签安装包已嵌入 Authenticode 签名，但 Windows 仍可能显示未知发布者或信任提示；本机自签模式不接受 `-TimestampUrl`。`AppPublisher` 字段不代表证书受信任。现有 Ed25519 PEM 密钥继续用于升级清单，不能代替 Windows 代码签名证书。

ISS 使用名为 `cohavora` 的签名工具，由打包入口通过 ISCC 的 `/Scohavora=<命令>` 提供；缺少工具或签名失败都会停止编译，不回退到未签名安装包。未来取得受信任证书后，可给入口传入 `-CertificateThumbprint`，并同时提供 RFC 3161 时间戳地址 `-TimestampUrl`，或使用下面的直接编译方式。

在 Inno Setup IDE 中直接编译 ISS 前，需要先配置同名工具。使用下面的命令复用本机自签证书并写入当前用户的 IDE 签名工具配置；该模式仅配置工具，不打包。配置后关闭并重新打开 IDE，再编译 `CohavoraInstaller.iss`。命令行的 `/Scohavora=...` 不会自动保存到 IDE，这是直接编译时出现 `SignTool is invalid` 的原因。

```powershell
& .\packaging\windows\build-installer.ps1 -ConfigureIdeSignToolOnly `
    -Iscc 'C:\Program Files (x86)\Inno Setup 7\ISCC.exe' `
    -SignTool 'C:\Program Files (x86)\Windows Kits\10\bin\<SDK版本>\x64\signtool.exe'
```

也可在 IDE 的 `Tools → Configure Sign Tools…` 中添加名为 `cohavora` 的工具，命令使用下面构造的 `$SignCommand` 内容。该配置保留其他已定义的签名工具；更换证书或 SignTool 路径后需要重新配置。

下面以 Windows 当前用户 `My` 证书库和 Microsoft SignTool 为例。在仓库根目录执行，先替换工具路径、证书指纹及证书服务商提供的 RFC 3161 时间戳地址。`/sha1` 用于精确选择证书，文件摘要和时间戳摘要均使用 SHA-256。私钥及密码不写入 ISS、源码或命令示例：

```powershell
$iscc = 'C:\Program Files (x86)\Inno Setup 7\ISCC.exe'
$SignTool = 'C:\Program Files (x86)\Windows Kits\10\bin\<SDK版本>\x64\signtool.exe'
$CertificateThumbprint = '<代码签名证书的40位十六进制指纹>'
$TimestampUrl = '<RFC3161时间戳服务地址>'
$SignCommand = '$q{0}$q sign /sha1 {1} /s My /fd SHA256 /tr $q{2}$q /td SHA256 $f' -f `
    $SignTool, $CertificateThumbprint, $TimestampUrl
$AppBuildDir = (Resolve-Path 'out/build/windows-vs2026-release/src/app/Release').Path
& $iscc "/Scohavora=$SignCommand" "/DAppBuildDir=$AppBuildDir" packaging/windows/CohavoraInstaller.iss
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup 编译失败' }
```

`$q` 和 `$f` 是 Inno Setup 签名命令占位符：`$q` 表示引号，`$f` 已包含待签文件路径的引号。上面使用单引号格式字符串，避免 PowerShell 提前展开占位符；云签名工具同样通过 `/Scohavora=` 传入命令并以 `$f` 接收文件路径。证书位于本机计算机证书库时按工具配置增加 `/sm`。签名失败不自动重试，避免重复凭据提示或远程签名请求。

直接编译 ISS 且未覆盖参数时，默认读取 `../live-kit-test-local/build-debug/src/app/Release/`，输出至 `out/installer/Cohavora-<版本>-x64-Setup.exe`；使用标准 Release 预设时须像上例一样显式传入 `AppBuildDir`。版本来自 `Cohavora.exe` 的版本资源，与构建该 EXE 时的 CMake `PROJECT_VERSION` 一致，无需在安装脚本中另行维护。缺少 EXE、任一渲染 DLL、版本资源或中文语言文件时，编译会明确报错。

ISCC 签署最终安装包、卸载程序及其需要的临时副本；打包前已构建的应用 EXE 和 renderer DLL 不会因此自动获得签名。卸载程序签名缓存保存在输出目录；更换证书或签名策略时使用新的输出目录。签名打包不要传入 ISCC 的禁用签名参数。使用受信任证书及时间戳后，用下列 Microsoft SignTool 命令验证最终安装包，所有非零退出码（含警告）均需处理；本机自签名且未加时间戳的包默认不会通过这项公众信任检查：

```powershell
$Installer = 'out/installer/Cohavora-<版本>-x64-Setup.exe'
& $SignTool verify /pa /all /tw $Installer
if ($LASTEXITCODE -ne 0) { throw '安装包 Authenticode 验证失败或存在警告' }
```

确认安装包的签名状态后，再使用 `packaging/updates/update_manifest.py create` 对最终安装包计算大小、SHA-256 并生成 Ed25519 签名的 `latest.json`；生成清单后不得再次签名或修改安装包。参考 [Inno Setup 签名工具配置](https://jrsoftware.org/ishelp/topic_setup_signtool.htm)、[Microsoft SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool)和 [New-SelfSignedCertificate](https://learn.microsoft.com/en-us/powershell/module/pki/new-selfsignedcertificate)。

脚本只打入上述三个运行文件、应用图标和项目 `LICENSE`，避免把构建目录内残留的 DLL、PDB 或其他文件带入安装包。当前标准配置静态链接 Qt、CRT 和其他应用依赖；如调整为动态依赖，需同步维护 CMake 和 Inno Setup 的文件清单。

支持以下预处理参数。默认路径相对于脚本位置，不依赖命令执行目录；覆盖路径时请使用绝对路径，并将整个 `/D名称=值` 参数放在双引号内，以支持路径中的空格。

| 参数 | 默认值／用途 |
| --- | --- |
| `AppBuildDir` | 直接编译 ISS 时默认为 `../live-kit-test-local/build-debug/src/app/Release/`；可覆盖为包含 EXE 和 `renderers/` 的运行目录，或干净的 CMake install 暂存目录 |
| `OutputDir` | 仓库下的 `out/installer/` |
| `ChineseMessagesFile` | Inno Setup 安装目录下的 `Languages/ChineseSimplified.isl` |
| `PackageCompression` | `lzma2/max`，兼顾体积与编译资源占用；本地快速验证可用 `lzma2/fast` |

例如对已经构建好的 Release 产物使用一个新的暂存目录打包：

```powershell
cmake --install out/build/windows-vs2026-release --config Release --prefix C:/packages/Cohavora-stage
if ($LASTEXITCODE -ne 0) { throw '生成打包暂存目录失败' }
& $iscc "/Scohavora=$SignCommand" "/DAppBuildDir=C:\packages\Cohavora-stage" "/DOutputDir=C:\packages\output" packaging/windows/CohavoraInstaller.iss
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup 编译失败' }
```

安装器支持简体中文／英文、桌面快捷方式和可选的登录自启动。默认安装到所有用户的 Program Files，也可在向导中选择仅当前用户，或传入 `/CURRENTUSER`；`/ALLUSERS` 显式选择全机安装。自启动注册项跟随安装范围，全机安装时对所有用户生效。

后续 Inno 安装包通过固定 `AppId` 在相同安装范围下原地升级，无需先卸载旧版。安装时由 Windows Restart Manager 处理占用文件，卸载前需关闭使用该安装目录的应用；脚本不会按进程名强制结束其他实例。卸载保留用户配置、凭据和会议数据。静默部署可使用 `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="安装日志路径"`；应用占用导致卸载无法继续时会返回失败，需关闭后重试。

如继续使用 CPack 的 NSIS 安装方式，安装 NSIS 3 后将 ZIP 命令中的 `-G ZIP` 改为 `-G NSIS` 即可；它与上述 Inno Setup 是独立入口，卸载信息不同，不应相互覆盖升级。

## 常见问题

**找不到 Visual Studio 18 2026 生成器。** 确认已安装 VS 2026 和支持它的 CMake。本文推荐 CMake 4.3 或更新版本；切换生成器时使用新的构建目录。

**找不到 Asio、Protobuf 或 spdlog。** 检查 `VCPKG_ROOT` 是否指向已完成 bootstrap 的 vcpkg，并确认首次配置可以安装 manifest 中的依赖。在首次配置前设置该变量。

**已经安装 Qt，为什么还提示缺少依赖？** 本项目使用固定的 Qt 5.15.18 静态 SDK 及配套库，普通 Qt 安装或 Qt 6 不能直接替代。先运行 `python build/prepare/prepare.py`；需要重编依赖时参阅 [依赖构建说明](build/prepare/README.md)。

**依赖下载失败，能使用本地压缩包吗？** 可以通过 `python build/prepare/prepare.py --qt-archive "C:\downloads\cohavora-libraries-win64-qt-5.15.18-20260925.zip"` 提供固定的 Qt/Libraries 归档。文件必须符合 [归档元数据](build/prepare/libraries-archive.json) 的校验信息；WebRTC 和 vcpkg 依赖仍需另外准备。

**可以离线配置吗？** 在仓库根目录执行 `python build/configure.py --offline`，但需要提前备齐子模块、Qt/WebRTC、vcpkg 工具和依赖缓存。它禁用 vcpkg 源站下载，不会自动补齐缺失依赖；`--offline` 与 `--prepare-deps` 不能同时使用。

**能启动界面，但无法加入会议。** 先确认使用的是 LiveKit 直连还是业务服务入会。直连需要正确的 WebSocket 地址和有效房间 JWT；业务模式需要正确的会议服务地址、账号权限和会议号。随后检查网络、服务状态及设备权限。

## 许可证

项目采用 **GPL-3.0-or-later**，详见 [LICENSE](LICENSE)。第三方依赖遵循各自的许可证。
