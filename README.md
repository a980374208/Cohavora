# Cohavora

开发文档与当前验收边界见 [项目文档索引](docs/README.md)；历史工程 PASS 不代表当前工作区或真实环境全部通过。

**基于 C++、Qt 和 WebRTC 的 Windows 原生音视频会议客户端，对接 LiveKit 实时音视频服务。**

Cohavora 提供桌面会议界面，也提供可单独运行的命令行示例。你可以用它加入音视频房间、共享屏幕、聊天和使用协作白板，也可以从源码了解原生客户端的设备采集、会议状态管理和视频渲染实现。

当前构建流程面向 **Windows x64**，使用 **C++20 + Qt 5.15.18**。账号登录、会议预约等业务功能需要兼容 OpenMeeting 的服务；只有 LiveKit 服务时，也可以通过地址和房间访问令牌直接连接。

- [开源依赖与致谢](#开源依赖与致谢)
- [主要功能](#主要功能)
- [快速开始](#快速开始)
- [加入第一场会议](#加入第一场会议)
- [开发与测试](#开发与测试)
- [代码结构](#代码结构)
- [打包应用](#打包应用)
- [常见问题](#常见问题)
- [许可证](#许可证)

## 开源依赖与致谢

感谢以下开源项目及其维护者。清单覆盖本仓库通过 CMake、vcpkg、Git 子模块和固定依赖包**明确引入或链接**的第三方库；同一个库即使通过多个渠道使用，也只列一次。

### 实时通信与通用组件

| 项目 | 在 Cohavora 中的用途 | 上游地址 |
| --- | --- | --- |
| WebRTC | 实时音视频和媒体传输；预编译包由 LiveKit 提供 | [WebRTC](https://webrtc.googlesource.com/src)、[LiveKit 构建包](https://github.com/livekit/rust-sdks) |
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

可选的 E2EE 互操作测试使用 [LiveKit C++ SDK](https://github.com/livekit/client-sdk-cpp) 的已有构建产物；它不是桌面应用的常规链接依赖。Qt、WebRTC 及其他依赖自身还可能包含传递依赖，实际分发时应以对应版本附带的许可证和声明为准。

## 主要功能

| 功能 | 说明 |
| --- | --- |
| 音视频会议 | 麦克风静音、摄像头开关、参与者列表与视频显示 |
| 设备设置 | 选择摄像头、麦克风和扬声器，预览摄像头并测试音频设备 |
| 屏幕共享 | 共享屏幕或窗口，支持共享批注 |
| 会议聊天 | 发送文字、图片和文件 |
| 协作白板 | 绘图、文本、多页、图片导入及 PNG 导出，支持主持方控制编辑权限 |
| 会议管理 | 登录、按会议号入会、快速会议、预约、编辑和取消会议，需要配套业务服务 |
| 连接与诊断 | 重连状态提示、音视频统计和本地诊断报告 |
| 界面语言 | 默认简体中文，可通过启动参数切换英文 |

白板内容保留在当前会议窗口中，需要留存时请在离会前导出。通讯录等部分导航入口尚未实现。底层另有 RPC、数据流和端到端加密能力，其中端到端加密可通过 `simple_room` 示例体验，桌面端尚无用户密钥配置入口。

## 快速开始

可选择 [AI 分步引导](#方式一ai-辅助构建) 或 [手动编译](#方式二手动编译)。完成构建后，参阅[加入第一场会议](#加入第一场会议)配置服务连接。

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

CMake 会通过该路径加载 vcpkg，并根据 [vcpkg.json](vcpkg.json) 安装 Asio、Protobuf、spdlog 等依赖。默认使用 `x64-windows-static`。

#### 3. 准备依赖并编译

在仓库根目录执行：

```powershell
python build/prepare/prepare.py
cmake --preset windows-vs2026-release
cmake --build --preset release --parallel 2
```

第一条命令下载并校验固定版本的 WebRTC 和 Qt/Libraries，将它们放入 `deps/`。Qt/Libraries 压缩包约 **1.22 GB**，首次准备需要网络和足够的磁盘空间；后续运行会检查并复用有效缓存。首次 CMake 配置还会通过 vcpkg 安装依赖。

这里使用 Release 预设，只构建桌面应用及其依赖，不构建测试和示例。`--parallel 2` 用于控制编译时的内存占用，可按机器配置调整。

#### 4. 启动应用

```powershell
.\out\build\windows-vs2026-release\src\app\Release\Cohavora.exe
```

需要英文界面时：

```powershell
.\out\build\windows-vs2026-release\src\app\Release\Cohavora.exe --language en_US
```

## 加入第一场会议

本仓库提供客户端。实际通话需要可访问的 LiveKit 服务；账号和会议管理还需要配套业务服务。

### 方式一：使用 LiveKit 地址和令牌直连

适合已有 LiveKit 服务、想先验证音视频的开发者。

1. 准备 LiveKit WebSocket 地址，例如 `wss://your-project.livekit.cloud`，以及服务端签发的有效房间访问令牌（JWT）。令牌决定可以进入的房间、参与者身份和权限。
2. 启动应用，在登录页选择“游客访问”，填写昵称进入主页。
3. 点击“加入会议”，展开“高级 LiveKit 连接”。
4. 填写 LiveKit 地址和访问令牌后加入；直连模式可不填写会议号。

“游客访问”创建的是本地访客身份，连接 LiveKit 仍需要有效令牌。多人测试时，为各参与者签发属于同一房间、身份不同的令牌。

### 方式二：使用账号和会议号

适合已经部署兼容 OpenMeeting 业务服务的环境。

1. 在登录页打开“服务器设置”，填写 HTTPS 会议服务地址；需要注册时，按部署情况配置注册服务地址。
2. 使用账号登录，创建或预约会议，或者输入已有会议号加入。
3. 客户端向业务服务申请入会，获取对应的 LiveKit 地址和令牌，再建立音视频连接。

会议业务服务地址与 LiveKit WebSocket 地址用途不同，请分别填写。HTTP 业务服务仅在以 `--debug` 启动的开发模式下允许使用。

## 开发与测试

以下操作沿用前面准备好的依赖和 `VCPKG_ROOT`。所有预设见 [CMakePresets.json](CMakePresets.json)。

### 调试桌面应用

```powershell
cmake --preset windows-vs2026-dev
cmake --build --preset dev-debug --parallel 2
.\out\build\windows-vs2026-dev\src\app\Debug\Cohavora.exe
```

`dev-debug` 只构建应用及其依赖。项目的 Debug 配置保留应用调试符号，但 Qt 和默认 WebRTC 依赖仍使用 Release ABI，运行库统一为 `/MT`；不要随意混入 `/MD` 或不同 ABI 的库。

### 运行核心回归测试

```powershell
cmake --preset windows-vs2026-core
cmake --build --preset core-debug --parallel 2
ctest --preset core-debug
```

该测试预设选择 `CORE_REGRESSION` 标签。核心测试配置仍包含 Qt 相关测试，因此需要 Qt/Libraries。

| 需求 | 配置预设 | 构建／测试入口 |
| --- | --- | --- |
| 开发桌面界面 | `windows-vs2026-dev` | `dev-debug` 构建应用；`all-debug` 构建全部默认目标 |
| 核心回归 | `windows-vs2026-core` | 构建与测试均使用 `core-debug` |
| 不依赖 Qt 的原生测试 | `windows-vs2026-native` | 构建使用 `native-debug`；CTest 使用该构建目录 |
| Release 应用 | `windows-vs2026-release` | 构建使用 `release` |

需要全量默认回归时，先在 dev 配置下执行 `cmake --build --preset all-debug --parallel 2`，再执行 `ctest --preset full-debug`。按改动范围选择测试即可，无需每次都运行全部测试。

真实设备、外部服务器和 GPU/交互桌面测试默认未开启，相关开关见 [tests/CMakeLists.txt](tests/CMakeLists.txt)。编译与自动化测试通过不代表真实网络、设备和长时间运行已经验证。

### 运行命令行示例

dev 配置提供三个示例目标：

| 目标 | 用途 |
| --- | --- |
| `simple_room` | 连接房间、发布示例音视频、观察参与者事件，可配置端到端加密 |
| `simple_rpc` | 演示参与者之间的 RPC 调用 |
| `cohavora_media_broadcaster` | 采集并发布麦克风、系统声音或摄像头媒体 |

例如先查看 `simple_room` 的用法：

```powershell
cmake --build out/build/windows-vs2026-dev --config Debug --target simple_room --parallel 2
.\out\build\windows-vs2026-dev\samples\Debug\simple_room.exe --help
```

示例支持 `--url`、`--token` 参数，也支持 `LIVEKIT_URL`、`LIVEKIT_TOKEN` 环境变量。`simple_room` 和 `simple_rpc` 未提供连接参数时仅显示帮助，不会验证实际连接。访问令牌属于凭据，请勿写入源码或提交到仓库。

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
| `samples/`、`tests/` | 示例构建定义与自动化测试 |
| `cmake/`、`build/prepare/` | 构建模块与依赖准备脚本 |

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

在仓库根目录执行，修改 `$iscc` 为本机实际安装路径；如果已加入 `PATH`，也可以直接使用 `ISCC.exe`：

```powershell
$iscc = 'C:\Program Files (x86)\Inno Setup 7\ISCC.exe'
& $iscc packaging/windows/CohavoraInstaller.iss
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup 编译失败' }
```

脚本默认读取 `out/build/windows-vs2026-release/src/app/Release/`，输出至 `out/installer/Cohavora-<版本>-x64-Setup.exe`。版本来自 `Cohavora.exe` 的版本资源，与构建该 EXE 时的 CMake `PROJECT_VERSION` 一致，无需在安装脚本中另行维护。缺少 EXE、任一渲染 DLL、版本资源或中文语言文件时，编译会明确报错。

脚本只打入上述三个运行文件、应用图标和项目 `LICENSE`，避免把构建目录内残留的 DLL、PDB 或其他文件带入安装包。当前标准配置静态链接 Qt、CRT 和其他应用依赖；如调整为动态依赖，需同步维护 CMake 和 Inno Setup 的文件清单。

支持以下预处理参数。默认路径相对于脚本位置，不依赖命令执行目录；覆盖路径时请使用绝对路径，并将整个 `/D名称=值` 参数放在双引号内，以支持路径中的空格。

| 参数 | 默认值／用途 |
| --- | --- |
| `AppBuildDir` | 标准 Release 预设的运行目录，包含 EXE 和 `renderers/`；也可指定干净的 CMake install 暂存目录 |
| `OutputDir` | 仓库下的 `out/installer/` |
| `ChineseMessagesFile` | Inno Setup 安装目录下的 `Languages/ChineseSimplified.isl` |
| `PackageCompression` | `lzma2/max`，兼顾体积与编译资源占用；本地快速验证可用 `lzma2/fast` |

例如对已经构建好的 Release 产物使用一个新的暂存目录打包：

```powershell
cmake --install out/build/windows-vs2026-release --config Release --prefix C:/packages/Cohavora-stage
if ($LASTEXITCODE -ne 0) { throw '生成打包暂存目录失败' }
& $iscc "/DAppBuildDir=C:\packages\Cohavora-stage" "/DOutputDir=C:\packages\output" packaging/windows/CohavoraInstaller.iss
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

**可以离线配置吗？** [configure.py](configure.py) 提供 `--offline`，但需要提前备齐子模块、Qt/WebRTC、vcpkg 工具和依赖缓存。它禁用 vcpkg 源站下载，不会自动补齐缺失依赖；`--offline` 与 `--prepare-deps` 不能同时使用。

**能启动界面，但无法加入会议。** 先确认使用的是 LiveKit 直连还是业务服务入会。直连需要正确的 WebSocket 地址和有效房间 JWT；业务模式需要正确的会议服务地址、账号权限和会议号。随后检查网络、服务状态及设备权限。

## 许可证

项目采用 **GPL-3.0-or-later**，详见 [LICENSE](LICENSE)。第三方依赖遵循各自的许可证。
