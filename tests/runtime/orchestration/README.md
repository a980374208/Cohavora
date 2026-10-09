# 长稳与共享诊断编排

公共入口为 `../tools/diagnostics/invoke_diagnostic_probe.ps1`。本目录保留各场景实现，公共入口负责参数路由、
互斥执行和历史输出保护；共用 ETW 工具，继续复用 meeting_soak、meeting_render_probe、
ecs_resource_sampler 和 livekit_signal_fault_proxy，不增加另一套采集器。

先用 `-Plan` 查看参数和脚本路径，不启动进程、不连接服务、不写文件：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/runtime/tools/diagnostics/invoke_diagnostic_probe.ps1 -Scenario share -PreparedDirectory out/probe-new -Instance i-YOURINSTANCE -ServiceUrl ws://YOURHOST:17880 -Binary out/build/windows-vs2026-runtime-tools/Release/test_screen_share_runtime.exe -CompleteLifecycle -Plan
```

PreparedDirectory 必须已存在；各运行使用独立目录。去掉 `-Plan` 才执行。
请用 `powershell -File` 启动独立进程，隔离探针环境变量。异常中断残留的
`diagnostic-probe.lock` 需先确认没有活跃进程，再手动移除。

| Scenario | 参数与用途 |
|---|---|
| soak | 可用 `-LowBandwidth`；先通过 `meeting_soak.py prepare --output ... --executable ...` 创建计划 |
| render | 必须显式 `-Binary`；`-Grid16Transport`、`-Grid16Transition`、`-NoSimulcast`、`-LowBandwidth`；原负载 grid16 还须 `-InputManifest`、`-Profile`；SSH 目标另传 `-TargetConfig` |
| recovery | 必须 `-Binary`；`-RecoveryMode recovery/full/lifecycle/diagnostic/network` |
| share | 必须 `-Binary`；支持 Cycles、FirstFrameOnly、DirectTrack、Performance、CompleteLifecycle、TraceHandles、WgcWindow、WgcScreen、Quality、Attribution、SampleSeconds、Codec |

不属于该场景的显式参数会报错。soak 拒绝覆盖已有 active-controller/active-result。
`-Python` 可指定解释器；实例和服务地址必须显式给出。默认继承受控 Workbench 环境约定：
Workbench 已配置，服务凭据位于目标 `/root/livekit.yaml`，远端具备 Python/LiveKit 工具，
服务为带显式端口的 ws origin；network 模式保留本机 17881/17882 故障代理端口。
render 可用 `-RemoteConfigPath` 指定其它远端配置路径，默认 `/root/livekit.yaml`；
`-SfuContainer` 默认 `livekit`；`-RemoteServiceUrl` 默认由公网 `-ServiceUrl` 的端口派生
`http://127.0.0.1:<port>`，远端管理 origin 不同时必须显式给出。
这三个参数及 `-InputManifest`、`-Profile`、`-TargetConfig` 仅适用于 render。
TLS 与反向代理不在此受控入口支持范围内。

render 的 SSH/Tencent 目标使用 `-TargetConfig` 指定本地 JSON，结构为
`schema=1`、`transport="ssh"`、`host`、`port`、`user`、`key_path` 和
`known_hosts_path`。host 必须为 IP 地址；两个文件路径须为绝对路径，known_hosts
须位于本仓库内。私钥仅由本机 SSH/ssh-keygen 使用，不上传或记录私钥内容；
冻结记录公钥指纹与 known_hosts 哈希。
实例标识支持真实腾讯云 `ins-*`；无法取得云端实例 ID 时，使用明确的
`ssh:<host>` 标识，不能把主机地址伪造成云实例 ID。其余场景仍只接受阿里云 `i-*`。
目标 SSH host key 应先通过可信来源核实并写入独立 known_hosts，认证和 host-key
检查失败时停止。目标主机身份、CPU/内存和公网带宽需分别保存真实证据；
认证失败或公网带宽 `UNKNOWN` 不能写成预检或资源 `PASS`。

## B11 grid16 输入冻结与诊断预检

render 不再使用隐式 Debug 二进制，必须通过 `-Binary` 显式选择经 PE CodeView
校验的 RelWithDebInfo `test_participant_window_remediation.exe`。
原负载 grid16 transport/transition 在任何 Workbench/SSH 查询、上传或远端启动前，调用
`meeting/b11_input_freeze.py verify`，核对 manifest、profile、探针源码及二进制输入。
当前 profile 为 `tests/runtime/tools/meeting/b11_grid16_preflight_profile.json`，
固定 17 路原负载发布、1 个接收端及 300 秒 grid16 transport 观察。
准备新实例时可以先冻结本地输入；profile 的 `instance`、`service_url`、
`local_service_url` 三项均为 null，冻结结果保持 `remote_target=PENDING`，
使用 `--require-remote` 的开测检查会拒绝这些未完成输入。
实例上线后复制 profile 到新目录，填写实际目标和配置并重新冻结；不修改本轮冻结的
profile，也不覆盖旧 manifest。
SSH profile 的根层另填写 `transport="ssh"` 和 `target_config` 路径，
配置文件与 profile 一起纳入冻结输入；未声明 transport 的旧 profile 继续使用 Workbench。

先用冻结工具生成 manifest，再离线验证（路径须替换为本轮实际文件）：

```powershell
New-Item -ItemType Directory out/b11-input-freeze-20261008 -ErrorAction Stop
Copy-Item tests/runtime/tools/meeting/b11_grid16_preflight_profile.json out/b11-input-freeze-20261008/profile.json
python -B tests/runtime/tools/meeting/b11_input_freeze.py freeze --output out/b11-input-freeze-20261008/input-freeze.json --executable build-debug/RelWithDebInfo/test_participant_window_remediation.exe --profile out/b11-input-freeze-20261008/profile.json
python -B tests/runtime/tools/meeting/b11_input_freeze.py verify --manifest out/b11-input-freeze-20261008/input-freeze.json --executable build-debug/RelWithDebInfo/test_participant_window_remediation.exe --profile out/b11-input-freeze-20261008/profile.json
```

远端信息补齐并重新冻结后，可先查看公共入口路由，以下 `-Plan` 不访问云端，
也不运行 manifest 验证或授予开测资格：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/runtime/tools/diagnostics/invoke_diagnostic_probe.ps1 -Scenario render -PreparedDirectory out/b11-grid16-new -Instance i-YOURINSTANCE -ServiceUrl ws://YOURHOST:17880 -Binary build-debug/RelWithDebInfo/test_participant_window_remediation.exe -Grid16Transport -InputManifest out/b11-grid16-new/input-freeze.json -Profile out/b11-grid16-new/profile.json -RemoteConfigPath /root/livekit.yaml -SfuContainer livekit -Plan
```

SSH/Tencent 目标将 `-Instance` 替换为真实 `ins-*` 或 `ssh:<host>`，并加入
`-TargetConfig out/b11-grid16-new/target.json`；该路径必须与冻结 profile 一致。

实例和服务地址必须与冻结 profile 一致。transport/transition 只能选择一个；
`-NoSimulcast` 与 `-LowBandwidth` 互斥，原负载模式须与 profile 一致。
历史 `-LowBandwidth` 为独立诊断，可不传 manifest/profile，但仍须显式
RelWithDebInfo 二进制；不将其结果迁移为 B11 原负载输入或验收。
本地 manifest 记录 `local_inputs=FROZEN`、`remote_target=PENDING`、
`binary_source_equivalence=UNKNOWN`、`runtime_status=NOT_RUN`；冻结仅证明输入记录一致，
不得据此声称 current-source build 或 B11 正式 PASS。诊断不授予正式放行信用。
grid16 帧推进预检也不替代百人容量、逐路媒体新鲜度和资源预算的正式判定门。

新的屏幕共享质量/热切换场景仍使用上一级的 `invoke_screen_share_quality_probe.py`、
`invoke_screen_quality_hot_runtime.py`、`analyze_screen_quality_soak.py`。
此入口主要保留原长稳、恢复、共享生命周期与归因场景，不替换产品级验收入口。

原始脚本与证据索引见 `../tool_migration.json`。此次整理不代表新的 L3 PASS。
