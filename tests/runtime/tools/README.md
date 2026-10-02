# Runtime 工具领域目录

| 目录 | 内容 | 主要入口 |
|---|---|---|
| `product_acceptance/` | 产品验收、采集、证据校验、云端传输及配套配置 | `invoke_product_external.ps1`、`verify_product_acceptance.py`、`run_b_acceptance_codec.py` |
| `meeting/` | 会议长稳、渲染探针、fake peer、低带宽发布与资源采样 | `meeting_soak.py`、`meeting_render_probe.py` |
| `screen_capture/` | WGC 长稳、屏幕共享质量采集与分析 | `run_wgc_soak.py`、`invoke_screen_share_quality_probe.py` |
| `desktop/` | 产品 UIA 监督、低频快照及 PowerShell worker | `product_uia_retest.py`、`uia_snapshot.py`、`e2ee_password_uia_worker.ps1` |
| `media/` | 双端媒体矩阵与 E2EE 互操作驱动 | `invoke_e2e_media_matrix.ps1`、`invoke_e2ee_interop.py`、`invoke_e2ee_product.py` |
| `diagnostics/` | 诊断场景统一参数入口 | `invoke_diagnostic_probe.ps1` |

入口继续支持按文件路径直接执行，CLI 参数和运行条件保持不变。使用仓库根目录作为工作目录，
具体命令与执行边界见 [runtime README](../README.md)、[UIA README](../../uia/README.md)
和 [编排 README](../orchestration/README.md)。

同领域 Python 模块及配套 JSON / worker 保持同目录；`desktop/product_uia_retest.py` 显式依赖
`meeting/meeting_soak.py`，屏幕质量驱动显式依赖 `product_acceptance/product_aliyun_transport.py`。
跨领域入口只添加所需目录，不扫描或自动加入全部领域目录。

远端部署的 `product_pilot_remote.py` 与 `product_pilot_context.py` 仍须一起复制，保持原来的远端文件名。
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
