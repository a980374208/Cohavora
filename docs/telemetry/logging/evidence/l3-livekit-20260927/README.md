# 真实 LiveKit 服务 L3 验证

日期：2026-09-27。结论：`VERIFIED_RUNTIME` 仅适用于下述同机双端屏幕共享、模拟重连及安全诊断事件落盘；它不代表完整产品 L3 或 Production Ready。HEAD `745af69d2ce2c154a17e72890a81784b21b4de67`，最终 tracked dirty patch 指纹 `de1cb30b8381df73cd3354d05f39083a7f14e60f`。保留工作区现有未提交修改；未 commit、push、发布或修改 Reference。

## 环境与凭据边界

- 客户端：Windows Debug `build-debug/Debug/test_screen_share_runtime.exe`，最终 SHA-256 `867364AA2FAB0AAAE287A6B8D05DF2C0C8DBEB94689E22F5F077E970D12795FF`。两身份在同一客户端进程、同一机器上连接独立测试房间；先前完整矩阵与 12 次循环使用当时记录的旧二进制，不将其冒充最终二进制的重新执行。
- 服务端：用户现有 ECS `i-2zefof0rekjzkbyi8c7j`，`livekit` 容器运行，`123.56.225.164:17880` TCP 可达。本次测试经显式开发开关使用 `ws://`；未验证 TLS、产品配置或真实后端协议。
- 每次测试从 ECS 配置生成两个 10 分钟令牌，仅通过临时权限为 `0600` 的文件下载并传给子进程环境。结果、命令和证据不包含令牌。最终本地/ECS 匹配的临时凭据文件均为 0；最终诊断段及对应 stdout 的 JWT/密钥模式扫描命中 0。探针与完整原始输出位于被忽略的 `out/soak-prepared/`，不作为可公开诊断包。

## 发现、根因与修复

- 初次恢复矩阵：全量重连后的摄像头恢复与反向共享通过，最后“共享时离会”前的再发布失败：两 Room 均 Connected，发送端本地共享为 Active，接收端无 publication/帧。完整矩阵在正常停止后的第二次共享也复现同一失败。失败仍记 FAIL，不回填 PASS。
- 服务端隔离房间快照仅见发送端 `CAMERA`。服务端接受 `SCREEN_SHARE` AddTrack 和正确的 MID→CID 映射，但共享 SDP 段 `sendonly` 且无 SSRC，track 长期 pending；服务端出现无法绑定的 RTP。发送端计数显示共享帧持续进入编码器并发送包、两个编码层 active。因此不是采集或接收 UI 问题。
- 根因：`RemoveTrackOrError` 后手动 `SetTrack` 复用同一 screen sender，WebRTC 仍输出 RTP，但新 publication 无法在 SFU 上绑定。禁用复用的受控对照通过；保留旧轨道、仅设 inactive 的候选仍失败。最终实现每次新建 screen transceiver，取消发布时 `RemoveTrackOrError` 后 `StopStandard`，交由协商回收旧段；摄像头 sender 保留。失败/无所属 transceiver 走明确 `StateUncertain`，不假报成功。
- 直接回归 `single_pc_negotiation_test` 校验每轮仅一个活动 screen sender、旧 sender 进入 stopping、摄像头不被接管。真实 12 次循环补充每次停止后原生 transceiver、活动 sender、capture/source/preview 回到基线的断言。
- 首次诊断落盘探针失败：`accepted=36 written=0 dropped_ordinary=15 dropped_critical=21 sink_failures=4 drain=3`。根因是探针在继承 `out/soak-prepared` 宽松 ACL 的目录下启动，而产品 sink 按私有目录契约拒绝写入。探针脚本改为先创建当前用户拥有、禁用宽松 ACL 继承的独占目录，不放宽产品 ACL。第二次事件已落盘，但脚本把 `DrainResult::Completed=1` 错判为 0；改为同时检查实际进程退出码、媒体标记、诊断汇总与段文件。直连 Room 探针绕过产品 admission，原先未注入匿名会话 ID；为两个测试 Room 分别注入随机匿名 ID 后重跑。失败与误判均保留，不回填 PASS。

## 最终结果

| 验证 | 结果 | 证据 |
|---|---|---|
| 当前源码 L2 build | PASS | `cmake --build build-debug --config Debug --target ALL_BUILD --parallel`，退出码 0；Visual Studio 18 2026 / MSBuild 18.5.4，Debug。 |
| 当前源码 L2 tests | PASS | `ctest --test-dir build-debug -C Debug --output-on-failure`，87/87 PASS，0 failed，273.02 秒；`CORE_REGRESSION` 40/40，`LOGGING_LG7` 4/4。 |
| 双端完整矩阵 | PASS | 房间 `logging-l3-20260927T054206Z-4933c2`；两次停止/再共享、发布中取消、窗口关闭、软恢复、全量重连、反向共享、共享时离会，8/8 场景 PASS；stdout SHA-256 `4A9B966027E6327CD118D968D3359708E9353E538B4147F17F0390E4D174E328`。 |
| 双端 12 次循环 | PASS | 房间 `logging-l3-20260927T054022Z-e706a0`；每次远端动画帧及取消发布确认，12/12 停止点原生 transceiver 数均为基线 `2`；stdout SHA-256 `ACD9E5E30D980EF01FDA0A6C7E27DFC44D6351C1D6AB2528D12A028B7451AA96`。 |
| 双端诊断落盘 | PASS | 最终房间 `logging-l3-20260927T060044Z-072ca4`，`run-recovery-service-probe.ps1 -Mode diagnostic` 进程/脚本退出码均 0；恢复、反向共享、共享时离会 3/3 PASS；`accepted=36 written=36 dropped_ordinary=0 dropped_critical=0 sink_failures=0 drain=1`，另有 1 条 writer 终态，段内共 37 条。stdout SHA-256 `F1C977B4900B98CDFC541AE455E069870A6C008390601FF611B9684376DB039E`；私有段 SHA-256 `78BDBE32F590DA8BC083674355A8FEB2220DE303ABFE3B9431095D52756C9F13`。 |
| 事件关联/隐私 | PASS（本次范围） | 37 条事件序列 1～37 无重复，1 个 run、2 个有效匿名会话 ID；32 条带 room generation 的事件均有 session ID，26 条有 operation ID。必需字段缺失 0，私有根目录 ACL 禁止宽松继承；JSONL 与对应 stdout 敏感模式命中 0，本地/ECS 临时凭据残留 0。 |
| settled 资源快照 | OBSERVED | baseline→第 12 次 settled：private bytes `25,702,400→32,309,248`（+6.30 MiB），句柄 `367→371`，线程 `21→20`，publisher transceivers `2→2`，活动 sender `1→1`，屏幕 capture/track/source/preview 均 `0→0`。这是单机短时观察，不替代 8 小时/100 次预算验收。 |
| 格式/隐私 | PASS（本次范围） | `git diff --check` 退出码 0；最终两份 stdout 敏感模式命中 0，临时凭据清理完成。 |

源码 SHA-256：`room.cpp` `B67F0EE62043ED820B8B9FCA238802910786E0C82A289234CBE46E255DE66AEC`；`room.h` `91F9411C7576A839A6CC3906DD872A582418BDA653EF2DFD1A4915BC1460B1BB`；`desktop_capture.cpp` `CD8EA07D3DD23F48A9EE6E9356AFD7E6AF6856DFD78F06C024865447F65CA049`；`test_screen_share_runtime.cpp` `F3D1B6A090BB1BAB63CB9390AC921849EC1DEF78C2F63B13C0C76B66140EF4F7`；`test_single_pc_negotiation.cpp` `C47753A06CD18FFEC0E560E84D228AA5DB3AF5A05811A4B8BFFA082B3A88CEDF`；本地探针脚本 `61F76E5FD7659928B4B6C8B9E716BA2A3AE1DCDB254438408AE2DB9F79FB1F19`。

仍 `DEFERRED/NOT_RUN`：跨物理设备、多用户产品 UI 操作、真实网络故障注入与媒体端点身份复盘、真实后端协议、TLS、慢盘/断电、8 小时与至少 100 次生命周期、Release 符号和独立人工支持包复盘。本次 `SimulateScenario` 的软/全量重连是服务端协商场景，不等同于真实网络故障；短时 private bytes 不是诊断功能相对于关闭状态的配对性能增量。其他 LG0～LG7 确定性证据保持原结论。
