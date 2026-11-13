# 依赖准备与打包

Windows 使用 `win.bat`，Ubuntu 使用 `ubuntu.sh`；两者转入对应平台的依赖准备入口。`--help` 只显示参数，不创建缓存、读取依赖清单或下载文件。

Windows Qt/Libraries 的固定源码、工具链和重编步骤见 [Libraries 重编指南](libraries-build-guide.md)。`libraries-archive.json` 只绑定该指南的本地指纹；本页的使用说明不参与依赖归档校验。已经发布的归档和重编指南资产继续使用各自的固定 SHA-256。

日常准备可复用本机文件属性匹配的哈希缓存。需要重新读取全部文件内容时，给准备入口加 `--verify-full`；首次安装和新候选目录始终完整校验。快速复用只用于本机未修改依赖，不能作为重新验收、发布或抗篡改的完整性证明。

默认自动下载的 SDK 由 `webrtc-package.json`（Windows）或 `linux-packages.json`（Ubuntu）固定。`webrtc-codecs-profile.json`、`e2ee-backend-codecs-profile.json` 是供 CMake 显式选择的本地配对包审查配置，不是可下载归档清单。使用对应 SDK 时，分别指定 `WEBRTC_ROOT`、`COHAVORA_WEBRTC_PACKAGE_PROFILE`、`COHAVORA_E2EE_BACKEND_PACKAGE_DIR` 和 `COHAVORA_E2EE_BACKEND_PROFILE`。H.265 解码还需 `COHAVORA_H265_RUNTIME_ROOT`；其默认配置为 `h265-runtime-profile.json`。这些配置不切换默认下载包，也不继承默认 SDK 的验证结论。

## Native E2EE backend overlay

E2EE 使用独立小型源码包与固定基础 SDK 配套；不替换 vendor 文件，不混用 Flutter 自定义音频 ABI。当前默认 profile 为 `e2ee-backend-profile.json`，基础 SDK 为 `webrtc-51ef663-cohavora-bssl-dual-v3`。

从已按固定上游源码生成的 repair/guard 输入打包：

```powershell
python build/prepare/package_e2ee_backend.py --patched <reviewed-patched-directory> --guard <reviewed-media-guard-directory> --bsd-license <WebRTC-LICENSE> --apache-license <Apache-2.0-LICENSE> --output <new-output-directory>
cmake -S . -B <build-dir> -DCOHAVORA_E2EE_BACKEND_PACKAGE_DIR=<absolute-extracted-package-directory>
cmake --build <build-dir> --config RelWithDebInfo --target cohavora_app
```

包内保留 WebRTC BSD、Apache-2.0 许可证、原源码版权头、修改说明和固定 profile。输入须与 profile 中的固定上游版本和 SHA-256 匹配。ZIP 使用固定时间戳和排序，输出目录必须不存在，避免覆盖已有产物。此工具只产生本地归档，不上传或发布。

CMake 对照仓库内 profile 验证每个包输入，并验证基础 SDK Debug/Release 库 SHA-256；缺失或不匹配立即停止配置。该入口优先于两个独立输入目录参数。BoringSSL 前缀仍只作用于修复 object，应用 ASIO/OpenSSL 边界保持不变，object 仍直接加入最终链接，避免归档优先解析回原实现。未配置 overlay 的产品不能据此宣称支持 Required 媒体 E2EE；Room 能力准入保持拒绝，不降级明文。


overlay 包含 `cohavora_iv_sequence.h`：发送媒体和数据共享进程级、CSPRNG 初始化的 96 位递增 IV 分配器；Room/cryptor 重建和重装相同密钥不会重置进程分配状态。随机源失败或每进程总计 2^32 次额度耗尽时拒绝产生新的密文，没有明文回退。多个独立进程仍依赖随机起点的概率隔离，不宣称全系统绝对唯一或跨设备总调用次数已被集中管理。该实现保留 12 字节 IV 与接收解析，不改变协议格式。输入须使用当前 profile 对应目录，不把其他 `patched/` 目录自动视为当前输入。
