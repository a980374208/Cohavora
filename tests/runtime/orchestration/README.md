# 长稳与共享诊断编排

公共入口为 `../invoke_diagnostic_probe.ps1`。本目录保留各场景实现，公共入口负责参数路由、
互斥执行和历史输出保护；共用 ETW 工具，继续复用 meeting_soak、meeting_render_probe、
ecs_resource_sampler 和 livekit_signal_fault_proxy，不增加另一套采集器。

先用 `-Plan` 查看参数和脚本路径，不启动进程、不连接服务、不写文件：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/runtime/invoke_diagnostic_probe.ps1 -Scenario share -PreparedDirectory out/probe-new -Instance i-YOURINSTANCE -ServiceUrl ws://YOURHOST:17880 -Binary out/build/windows-vs2026-runtime-tools/Release/test_screen_share_runtime.exe -CompleteLifecycle -Plan
```

PreparedDirectory 必须已存在；各运行使用独立目录。去掉 `-Plan` 才执行。
请用 `powershell -File` 启动独立进程，隔离探针环境变量。异常中断残留的
`diagnostic-probe.lock` 需先确认没有活跃进程，再手动移除。

| Scenario | 参数与用途 |
|---|---|
| soak | 可用 `-LowBandwidth`；先通过 `meeting_soak.py prepare --output ... --executable ...` 创建计划 |
| render | `-Grid16Transport`、`-Grid16Transition`、`-NoSimulcast`、`-LowBandwidth` |
| recovery | 必须 `-Binary`；`-RecoveryMode recovery/full/lifecycle/diagnostic/network` |
| share | 必须 `-Binary`；支持 Cycles、FirstFrameOnly、DirectTrack、Performance、CompleteLifecycle、TraceHandles、WgcWindow、WgcScreen、Quality、Attribution、SampleSeconds、Codec |

不属于该场景的显式参数会报错。soak 拒绝覆盖已有 active-controller/active-result。
`-Python` 可指定解释器；实例和服务地址必须显式给出。当前继承受控环境约定：
Workbench 已配置，服务凭据位于目标 `/root/livekit.yaml`，远端具备 Python/LiveKit 工具，
服务为带显式端口的 ws origin；network 模式保留本机 17881/17882 故障代理端口。
TLS、反向代理或不同远端配置布局不在本次迁移支持范围内。

新的屏幕共享质量/热切换场景仍使用上一级的 `invoke_screen_share_quality_probe.py`、
`invoke_screen_quality_hot_runtime.py`、`analyze_screen_quality_soak.py`。
此入口主要保留原长稳、恢复、共享生命周期与归因场景，不替换产品级验收入口。

原始脚本与证据索引见 `../tool_migration.json`。此次整理不代表新的 L3 PASS。
