# 持续会议长稳准备与运行

`meeting_soak.py` 是独立进程 watchdog，驱动 `test_participant_window_remediation --meeting-soak`。
它保持同一个 Qt 进程和逻辑会议会话连续运行；SDK full reconnect 可以替换 native Room generation。
默认时长为 **30 分钟稳态 + 120 分钟混合**。准备和短时故障注入测试不连接 SFU，不算 L3 通过。

## 准备（无需服务或设备）

从仓库根目录使用 Python 3.10+，仅需标准库：

```powershell
python tests/runtime/meeting_soak.py prepare --output out/soak-prepared
```

生成 `profile.json`、`plan.json` 和 `preparation.json`，状态为 `PREPARED / L3 NOT_RUN`。
目录禁止覆盖。默认使用 `out/build/windows-vs2026-dev/Debug/test_participant_window_remediation.exe`；
可以用 `--executable` 指定其它已构建、ABI 匹配的 acceptance 可执行文件。
`--mixed-minutes` 可以延长混合阶段，但不能小于 120。正式运行没有短时覆盖开关。

内存阈值应按固定 DUT 的首轮基线制定。默认只观测、不虚构容量阈值：
即使调度完成，未设置阈值时总体 `INCONCLUSIVE`，子结论 `schedule_status=PASS`。
确认阈值后可在 profile 中同时填写 `max_growth_mib` 和 `max_slope_mib_per_hour`，
也可以准备时同时传入 `--max-growth-mib`、`--max-slope-mib-per-hour`。
两者任一超过上限即失败；这表示资源回归告警，不单独证明内存泄漏根因。

## 正式运行前的条件

- Windows 交互桌面持续解锁；至少预留 **2.5 小时，加上连接和退出余量**。
- 测试期间不休眠、不锁屏、不自动更新重启；工具不会修改机器电源或更新策略。
- 在本机环境中配置 `LIVEKIT_URL` 和 `LIVEKIT_SOAK_TOKEN`，凭证有效期覆盖整个测试窗口。
  Token 需要入房、订阅，以及发布受控窗口共享的权限。不要将凭证放进命令行或证据文件。
- 同一个真实 SFU 房间持续有至少 **17 个独立参会者发布允许订阅、未 mute 的摄像头视频**，并持续产生新帧。
  这个数量保证 16 格之外仍有可翻页内容；可用 profile 的 `min_remote_videos` 提高负载。
  本工具不创建远端发布者，不等同百人负载发生器。
- 默认不打开本机摄像头或麦克风。共享动作只捕获工具自己的动画测试窗口。
- 默认遵循生产服务安全策略；仅本地开发 SFU 需要时显式设置 `LIVEKIT_SOAK_ALLOW_INSECURE=1`。
- 建议发布负载放在独立机器；固定 DUT、驱动、codec、分辨率、FPS、负载规模及网络条件。

用户预留测试窗口并明确要求继续后，才运行：

```powershell
python tests/runtime/meeting_soak.py run --prepared out/soak-prepared
```

每次自动生成独立的 `runs/<UTC>-<id>/`，不覆盖此前结果。缺可执行文件或服务环境返回 `NOT_RUN`。

## 调度及判定

启动先等待真实 catalog 满足远端负载要求、runtime telemetry 有进展、9 格计划接受并完成媒体与绑定收敛，
**此后才开始 1800 秒稳态计时**。混合阶段每 10 分钟一个周期，默认重复 12 次：

| 周期偏移 | 动作 |
|---|---|
| 0 秒 | 9 格；内存可比检查点 |
| 50 秒 | 4 格 |
| 100 秒 | 下一页 |
| 150 秒 | 16 格 |
| 200 秒 | Pin 当前有效远端视频 |
| 250 秒 | Unpin |
| 300 秒 | 打开白板 |
| 350 秒 | 返回视频 |
| 400 秒 | 发布工具自己的动画窗口共享 |
| 450 秒 | 停止共享 |
| 500 秒 | SDK soft reconnect |
| 550 秒 | SDK full reconnect |

每个动作有单调序号；adapter 在实际生产状态满足动作后 ACK。
watchdog 还等待 selected/bound 收敛和新解码/呈现提交，白板阶段则要求视频选择与绑定清零。
序号、run id、PID 不匹配、动作拒绝、容量越限、计划迟迟不收敛均失败。
混合动作不会在睡眠后一次性补发并冒充完成规定时长。

- Qt 心跳：1 秒一份原子状态，10 秒无进展判 UI 卡死。
- runtime 心跳：来自 session strand 的真实 telemetry revision，15 秒无进展判 runtime 卡死。
- 媒体进度：可见视频解码与真实 render submit 分别监控，20 秒无进展失败；
  当前 stream/probe 汇总计数降低时记录 rebase，降低本身不计为进展。
- 稳态持续检查最低远端负载、连接状态与视频绑定；临时变化容忍 15 秒，最终状态必须健康。
- 动作收敛超时 40 秒；退出超时 30 秒。提前退出即使退出码为 0 也失败。
- 正常结束需 stop ACK、shutdown 完成、窗口/render/native 资源释放、四层全零且退出码为 0。
- 锁屏、长采样空洞、取消、证据写入故障标 `INCONCLUSIVE`。失败或超时只终止本次启动的进程。

内存每秒读取 DUT 的 Windows `PrivateUsage`（Private Bytes）、Working Set 和句柄数。
稳态排除前 5 分钟；混合阶段只比较每周期回到 9 格且收敛的检查点，避免布局分配台阶。
一分钟桶取中位数，再计算成对斜率中位数和首尾窗口增长；要求至少 3 个桶、120 秒跨度、
95% 有效采样覆盖及有效尾部数据，否则 `INSUFFICIENT_DATA`，不接受为内存通过。

## 证据与状态

每次运行产生：

- `run.json`：运行身份、起始时间、源码 HEAD/关键输入 SHA-256、adapter 二进制 SHA-256。
- `profile.json` / `plan.json`：实际使用的参数与完整计划。
- `events.jsonl`：动作、收敛、watchdog、计数重置和失败分类，逐行写出。
- `metrics.csv`：内存/句柄、状态、policy revision 和 requested/selected/actual/bound 时序。
- `last-status.json` / `summary.json`：最后有效状态、退出码、完成数、实际时长、内存判定。
- `manifest.json`：归档文件的 SHA-256 与大小；同级 `.zip` 和 `.zip.sha256`。

归档采用明确文件清单，不收集环境变量、token、任意 SDK stdout/stderr、桌面图像或崩溃内存。
若 watchdog 自身被强制结束或机器重启，保留的 `RUNNING` 清单只能判未完成，不能判 PASS。
退出码：0 = 本次声明范围 L3 PASS；1 = FAIL；2 = NOT_RUN 或 INCONCLUSIVE。

`actual` 是当前可用的媒体 binding 集合，可能保留退选轨道的绑定；它不是活跃 RTP 或 decoder 数量，
不能用 `actual <= 16` 或 `actual == selected` 推断 SFU 已停流。硬预算作用于 selected/bound。
音频帧目前为 null，音频连续性、GPU 显存、物理断网、百人容量不属于本工具已经测量的范围。
共享检查本端真实 Active/Idle 生命周期，尚未通过另一个接收端确认共享媒体到达。
soft/full reconnect 是真实 SFU 上的 SDK 故障请求，不冒充路由器或网卡物理断网。

## 无服务短时验证

```powershell
python tests/runtime/test_meeting_soak.py -v
out/build/windows-vs2026-dev/Debug/test_participant_window_remediation.exe --meeting-soak-protocol-selftest
```

Python fake peer 只用于秒级故障注入，所有结果强制 `l3_status=NOT_RUN`。
协议 selftest 不创建真实会议、不打开摄像头、不捕获桌面。
既有窗口生产控制仍由 `meeting_video_viewport_render_lease_test` 回归覆盖。
