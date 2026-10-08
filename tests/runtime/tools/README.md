# Runtime 工具领域目录

| 目录 | 内容 | 主要入口 |
|---|---|---|
| `product_acceptance/` | 产品验收、采集、证据校验、云端传输及独立诊断 | `invoke_product_external.ps1`、`verify_product_acceptance.py`、`run_b_acceptance_codec.py`、`invoke_tencent_pilot.ps1` |
| `meeting/` | 会议长稳、渲染探针、fake peer、低带宽发布与资源采样 | `meeting_soak.py`、`meeting_render_probe.py` |
| `screen_capture/` | WGC 长稳、屏幕共享质量采集与分析 | `run_wgc_soak.py`、`invoke_screen_share_quality_probe.py` |
| `desktop/` | 产品 UIA 监督、低频快照及 PowerShell worker | `product_uia_retest.py`、`uia_snapshot.py`、`e2ee_password_uia_worker.ps1` |
| `media/` | 双端媒体矩阵、E2EE 互操作驱动与独立音源旁证 | `invoke_e2e_media_matrix.ps1`、`invoke_e2ee_interop.py`、`invoke_e2ee_product.py`、`microphone_input/probe_microphone_input.py` |
| `diagnostics/` | 诊断场景统一参数入口与进程内 GPU 预算旁证 | `invoke_diagnostic_probe.ps1`、`gpu_budget/` |

入口继续支持按文件路径直接执行，CLI 参数和运行条件保持不变。使用仓库根目录作为工作目录，
具体命令与执行边界见 [runtime README](../README.md)、[UIA README](../../uia/README.md)
和 [编排 README](../orchestration/README.md)。

同领域 Python 模块及配套 JSON / worker 保持同目录；`desktop/product_uia_retest.py` 显式依赖
`meeting/meeting_soak.py`，屏幕质量驱动显式依赖 `product_acceptance/product_aliyun_transport.py`。
跨领域入口只添加所需目录，不扫描或自动加入全部领域目录。

`meeting/meeting_soak.py prepare` 默认保留 1800 秒 steady、至少 7200 秒 mixed
及至少 17 路远端视频；`--diagnostic`（别名 `--smoke`、`--allow-short`）允许短测
或省略 mixed。诊断与 self-test 的 L3 为 NOT_RUN；所有 standalone soak 均记录
release_eligible=false、qualification_credit=0，不授予 B14 资格或正式放行信用。
真实运行仅接受经 PE CodeView 校验的 RelWithDebInfo 二进制。

`product_acceptance/run_product_first_cycle_media_log_diagnostic.ps1` 只编排一次
产品启动和首周期媒体／日志对照，复用既有十路发布负载及远端退出清理。
`invoke_product_first_cycle_media_log_diagnostic.ps1` 从冻结 UIA 源码提取原动作；
`verify_product_first_cycle_media_log.py` 在新目录复核真实媒体、日志性能和退出证据。
运行预算 600 秒，输入活动仅记录；所有输出均为诊断、qualification_credit=0，
不执行共享、完整 GPU 门、三轮 PILOT 或正式长稳，不证明物理麦克风持续非静音。

`product_acceptance/invoke_tencent_pilot.ps1` 是独立诊断入口，使用专用
PowerShell 7.5+ 的 `pwsh.exe -NoProfile -File` 进程。必须显式传入 `-PreparedDirectory`、`-TargetConfig`、
`-Executable` 和 `-Root`（别名 `-EvidenceRoot`）；输出须为仓库 `out/` 下新目录。
TargetConfig 为 JSON：schema=1、provider="tencent"、service_url 与 PREPARED setup.json
的 URL 一致（server_provider 可作 provider 的别名）；URL 不接受 userinfo/query/fragment。
PreparedDirectory 包含 setup.json 和当前 Windows 用户可解密的 password.dpapi。
`-PrepareOnly` 只校验本地输入与二进制并生成诊断计划，不解密、不访问网络、不启动子进程。
真实诊断仅查询既有会议的身份、ownership 与有效期，随后复用 UIA/资源/归档采集；
密码只注入 UIA 子进程环境。无论是否启用 heap，都写入正式放行拒绝标记；
DIAGNOSTIC_ONLY_COMPLETE 仅表示本地诊断流程完整，云端负载、独立媒体与正式验收仍为 NOT_RUN。

远端部署的 `product_pilot_remote.py`、`product_pilot_context.py`、
`product_pilot_local_route.py` 与 `product_aliyun_target.json` 须一起复制，保持远端文件名。
`product_pilot_timing.py` 随采集器部署；仅 `-AudioTimingDiagnostic` 启用 FFI／事件循环时序。
`product_pilot_audio_reference.py` 同步部署；仅时序诊断在独立 PeerConnection 中接收既有合成音频，
同进程按真实 AudioStream handle 关联旁证，与产品 RTP／PCM 验收分离；不替代物理麦克风源确认。
不保存 PCM／像素／地址／凭据，不修改外部 SDK 文件；该轮显式标记 diagnostic_only、
release_eligible=false 并生成正式放行拒绝标记，任何诊断 PASS 都不能计入三轮。
`product_pilot_video_counter.py` 保持原生 VideoStream／解码／事件过滤和 4 项有界队列，
只计实际 native decoded-frame 事件，立即释放对应 buffer，不再复制未用于判定的像素。
保留全部实际 RTP／decoded stats 和帧推进门，并新增全部 video buffer 正常释放门；
它不证明像素质量。按当前固定 SDK 的私有 ownership 接口适配，升级 SDK 必须重验证。
`analyze_product_timing.py` 离线关联诊断时序与 PCM 超限，不生成 PILOT／正式 PASS。
`product_pilot_context.py` 在 CLI 丢失回执时只读恢复一次现有确认，严格核对
run／周期／operation 和服务端原 10 秒窗口；不重发操作、不接受迟到或缺证回执。
正式运行期间只做本地进程／采样尾部监控，远端完整证据搬运和复盘放在运行结束后，
避免额外管理流量和 CPU 工作污染固定负载及整机出口门。
离线自测在 `../selftests/`，真实桌面自测在 `../desktop_checks/`；工具目录不注册新的自动测试。

```powershell
python -B -m unittest discover -s tests/runtime/selftests -p "test_*.py" -v
```

工具运行涉及服务、设备、桌面、采集或长稳时仍需满足原有显式执行条件；离线自测通过不代表 runtime 验收通过。

`product_acceptance/run_b_acceptance_codec.py` 编排既有媒体矩阵，显式依赖
`media/invoke_e2e_media_matrix.ps1` 和 `screen_capture/invoke_screen_share_quality_probe.py`
的云端凭据传输。`--loopback-clock` 只用于同机时钟交换，媒体仍经过 ECS SFU；
`--idle-peers` 使用同目录 `server_idle_peers.py` 加入三个限时零媒体身份，以固定服务默认
人数 regression 条件，结束时核对关闭。每轮保留输入、二进制、服务配置和独立 case verdict。

`prepare_b_acceptance_server.py` 管理本任务标签的独立服务，保持共享 `livekit` 不变。
`configure_b_acceptance_network.py` 默认只读；写入只允许已核实的单个 IPv4 `/32` 和
TCP 17980/17981、UDP 17982，`--cleanup` 只删除任务标签对应的规则。

`screen_capture/run_capture_backend_matrix.py` 对自有动画窗口/覆盖主屏的图案执行
七项独立后端与实帧错误回退 case，并单独支持 complex-window 内容比较。显式依赖
同领域 `invoke_screen_share_quality_probe.py` 的凭据获取及其 product_acceptance 传输。
使用 RelWithDebInfo，记录 source/binary/server 指纹、真实 backend/远端帧和停止释放；
两个 native Room 在同一进程内，不作跨设备或跨进程保证。

`build-debug/RelWithDebInfo/test_desktop_capture_runtime.exe --screen-binding-observation 300`
用于远程浏览接入时的本机捕获诊断：选择与产品相同的第一个屏幕，使用生产捕获链，
每 500 ms 检查原屏幕绑定，输出帧计数、后端和固定错误原因码，不保存像素或设备名。
即使绑定随后恢复，发生过的错误仍保留；不检查用户输入，也不计正式长稳或媒体放行。

`complex-wgc-window` / `complex-gdi-window` 保留原 PSNR 30 dB、SSIM 0.95、
文字区域 PSNR 30 dB 门。`cost-wgc-window` / `cost-gdi-window` 另行运行同样的
生成窗口，关闭逐秒像素比较，记录 10 秒 camera-only 基线及 30 秒共享负载的
进程 CPU、实际 screen RTP 编码/解码/字节增量；核对轨道与实际后端。
成本采集 PASS 只表示计数完整且媒体推进，不声明未经规定的 CPU 或带宽预算通过。

`desktop/run_b_acceptance_uia.py`串行运行原UIA独立场景并记录二进制/原判定器指纹；
`product_acceptance/run_b_acceptance_files.py`使用两个真实产品Coordinator进程验证文件拒绝、
上传/SHA256落盘回读、中断/重试和Qt/session strand队列/native释放，不替代物理断电。
`meeting/run_b_acceptance_whiteboard.py`使用三个独立产品进程验证八个B12子场景，
显式依赖product_acceptance的文件peer夹具/服务snapshot与screen_capture认证传输；
authority图片权限、完整文档/sequence/asset及Pillow独立导出像素均单独核对。
三个进程在同一Windows主机，不代表三台物理终端。

`product_acceptance/run_b_acceptance_network.py`独立编排B10的11个网络/服务恢复case；
`configure_b_acceptance_fault_network.py`仅在任务容器namespace注入并恢复故障。
`configure_b_acceptance_turn.py`核对真实Room参与者，管理任务UDP TURN配置、
客户端/32临时规则和直连阻断；结束核对规则撤销与原配置恢复。

`product_acceptance/run_b_acceptance_telemetry.py`使用真实产品窗口、原生回调计时及
进程资源计数，执行三组交替顺序的同媒体负载配对和至少9000秒资源长门。
关闭基线实际停止strand采样、历史持久化及日志生产；性能窗口不查询UIA。
`B_TELEMETRY_STAGE_TIMING=1`仅用于单独分段诊断，不替代正常计时输入。
`run_b_acceptance_telemetry_lifecycle.py`另行执行20轮UIA查询/客户端退出对照和
同进程5次join/leave资源残余门，显式依赖同领域telemetry/network/files驱动及
`desktop/run_b_telemetry_uia_interference.ps1`。每轮核对Room与清理服务归零、
真实媒体推进和history/diagnostic排空；限定门不替代B14的8小时/100次验收。

`product_acceptance/run_b14_acceptance.ps1` 在独占桌面执行三轮完整 PILOT，
经既有正式放行判定器核对 current-source 验证和冻结输入后，再从零执行
8 小时/100 次产品长稳。必须显式传入 RelWithDebInfo 产品与音频采集器，
证据磁盘预算按实际输出盘检查；运行期间持有任务锁和系统/显示唤醒请求，
终止后释放。PILOT 失败即停止，历史失败与 DEFERRED 边界保持原样。
checkpoint 归档采用逐段 gzip 无损存储；原生 SHA-256、大小、revision 1
到终态的检查不变，并额外核对压缩文件哈希及大小。原有归档段存储预算
保持 PILOT 1 GiB／正式 32 GiB，同时记录解压后原始字节数；旧原始段兼容。
同机测试 peer 的媒体通过独立 net_cls cgroup 和端口限定 OUTPUT DNAT 留在本机；
只匹配本轮采集子进程、SFU 公网地址与 RTC UDP/TCP 端口，结束按原规则撤销。
SFU 配置、共享进程、负载及整机 eth0 出口容量门不变；配置哈希、本地路由、
规则命中和正常清理作为独立证据。原生全局清理计数只在本周期离会后的持续
释放窗口归属，下一周期入会和进程退出另行核对，不用旧 SID 归属新入会作业。

`product_acceptance/product_gpu_queue.py` 核对真实产品 PID 的全部 WDDM
scheduler-node packet 原始计数、在会工作推进和完整周期采样；scheduler
nodes 与 DXGI/D3D12 memory nodes 分开计量，adapter 别名不相加。
`product_gpu_queue_diagnostic_policy.json` 的 64／4／0 是用户要求的大概
诊断界限，由 `-GpuBudgetDiagnostic` 在运行前复制并验哈希；不作为正式
GPU 放行。查询失败和 counter 反序／reset 保持 UNKNOWN／FAIL，不能
将 device 销毁后的 unavailable 补零或作为释放成功。

`diagnostics/gpu_budget/invoke_gpu_release_control.ps1` 编排独立 copy/fence／
device 释放对照，用 `product_gpu_trace` 的独立 DxgKrnl ETW session 保留
提交、完成和 context/device 销毁。`product_acceptance/product_gpu_etw.py`
验证 payload 所有权链、配对、时钟与零丢失；独立 PASS 无资格／正式信用。
`invoke_gpu_live_control.ps1` 另验证实时消费及延迟 PID 绑定，绑定前全部事件
保留，绑定后保留全部设备／上下文生命周期和所有权链关联的 packet，
header PID 不作工作归属。B14 编排现在在产品启动前准备实时采集器，正常
退出后停止并排空，保存 loss／解析／字节预算及四采集器退出证据。
离线复核只保留当前 ownership／pending 状态和预声明周期窗口，避免按
8 小时事件数累积 transition 列表。历史诊断继续使用暂定 8 GiB/64／4／0，
不计资格。`product_gpu_queue_limits.json` 根据用户授权选择测试数值，
在新普通资格前冻结本 B14 环境的 WDDM+ETW scheduler packet 64／4／0，
并要求释放窗口 context/device 为零；保留 hardware queue depth 边界。
普通资格与正式必须使用独立 32 GiB GPU 日志上限，启动时至少 72 GiB
空闲（另含原 32 GiB checkpoint 与 8 GiB余量），哈希绑定三轮输入和
正式门；未知/缺失/丢失/终态不配对持续失败，诊断标记持续拒绝资格。
