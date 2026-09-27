# 媒体端点身份与真实信令故障 L3

日期：2026-09-27。状态：**PASS（受控真实 LiveKit 信令故障、同机双端）**。契约见 `../../MEDIA_ENDPOINT_IDENTITY_CONTRACT.md`。HEAD `745af69d2ce2c154a17e72890a81784b21b4de67`，工作区有未提交改动；未 commit、push、发布或修改 Reference。原始运行结果与代理日志位于被忽略的 `out/soak-prepared/`，私有诊断段由仅当前用户可访问的 ACL 保护。

## 失败、修复和当前结果

| 运行/房间 | 结果 | 解释 |
|---|---|---|
| `logging-l3-20260927T065425Z-bce75a`、`logging-l3-20260927T065739Z-401405` | FAIL | 代理真实断开接收端 WebSocket 并拒连 3 秒；`reconnect_total=35s`，但 5 次快速连接失败约 2.6 秒就触发 `reconnect_exhausted`。原始 FAIL 文件保留。 |
| `logging-l3-20260927T070929Z-8c5947` | 包装器 FAIL，探针 PASS | 原始探针报告媒体恢复、44/44 写盘；PowerShell 包装器未取得进程退出码，没有将这轮回填为正式 PASS。修复为启动时保留进程句柄，并在 WaitForExit 后读取退出码。 |
| `logging-l3-20260927T071218Z-bc0b01` | PASS | 包装器与探针退出码均为 0，43/43 写盘；后续收紧历史包的 generation/operation 关联。 |
| `logging-l3-20260927T071821Z-2368c8` | **PASS，最终当前二进制** | 包装器/探针退出码 0，`[CASE] actual_signaling_network_fault PASS camera_recovered=true`；43/43 写盘、关键/普通丢失 0、sink 失败 0、drain 完成。运行二进制 SHA-256 `D1969290B6A1EEB5C5278878547AD31690B0F9F6E58221B8F547E2F4121C03E5`。 |

根因：`Room::AttemptReconnect` 同时受总 deadline 和固定 5 次尝试限制；TCP 快速拒连先耗尽次数，使 35 秒总预算失效。修复后按总 deadline 持续有界退避（100 ms 起、约 1 秒封顶，单次尝试仍受预算限制），取消与 session generation 检查保留。确定性测试把本地服务关闭 3.2 秒再恢复，验证未提前发出最终断开并能重连。

最终运行：代理只绑定 `127.0.0.1:17881/17882`；发送端直连共享 ECS `i-2zefof0rekjzkbyi8c7j` 的 LiveKit，接收端走代理。摄像头有帧后，控制命令关闭 1 条活动连接，按单调时钟拒连 3 秒；代理记录被阻断连接及窗口结束后的新上游连接。安全 JSONL 共 44 行：1 个 reconnect episode started/terminal，6 个 attempt terminal（5 failure、1 success），1 个 `media.endpoint.recovered` 与 1 个 `media.recovery.milestone`。episode 到稳定视频端点事件 4235 ms。两事件同属匿名 session、operation `reconnect_episode:1:2`、recovery epoch 1，`expected_endpoints=1`；32 字符旧/新端点 ID 不同，测量点为 `decoded_video_stably_recovered`。这些 ID 仅表示本机 native 消费绑定，不表示远端身份、IP、ICE 或 SFU。

私有 JSONL 与探针 stdout 的只读扫描：JWT 形态、WebSocket URL、IPv4、测试身份和房间名均未检出。退出后本地临时凭据 0、ECS `/tmp/logging-l3-*-credential.json` 0，代理监听端口 17881/17882 均为 0。未改 ECS 服务配置或防火墙。

## 工程验证

- `cmake --build build-debug --config Debug --target cohavora_app test_stress_lifecycle test_telemetry_report test_screen_share_runtime --parallel`：PASS；后续仅历史包 parser/test 更新后，`cohavora_app test_telemetry_report test_screen_share_runtime` 重建 PASS。Visual Studio 18 2026 / MSBuild 18.5.4，Debug，当前工作区的 `build-debug`。
- `ctest --test-dir build-debug -C Debug -R '^(stress_lifecycle_test|telemetry_report_test)$' --output-on-failure`：2/2 PASS。parser/test 最后改动后仅受影响的 `telemetry_report_test` 重跑 1/1 PASS；stress 输入未变，复用已通过结果。历史包测试覆盖缺失、同 epoch 不同 session generation、完整端点证据和非法 ID。
- `git diff --check`：退出码 0。未在当前代码上重复旧 87/87 Full CTest；旧 L2 是此前 LG7 Snapshot，不作为本次新代码的测试结果。

输入 SHA-256：`room.cpp` `8FD6405705400A379F030C205DE0DB08DC14AC66D5C429B7A5CDFDAD85E1A5C8`；`session_telemetry.cpp` `7DE28CEF805A828124A65834FB80C990A0AF6577997C305F5E18E8FF73FE6A9C`；`diagnostic_bundle.cpp` `E0083AFC25A0736338D3AA231F31E36141795B6EFCB56D315C039EAA965E5C73`；`telemetry_operation_timeline.cpp` `90D0406FA08AE467E87B5098D8DB87317AA8D06BC2C1FABA89A02B32A370B2A5`；`test_stress_lifecycle.cpp` `806C61426B23DBB3D834335A1BDC983ACD1756CCEF415E78BDDCDAEEFCEDE025`；`test_telemetry_report.cpp` `E09B3AD90728FA23ED75CBEE0179709FFB04259421E91B20A0EB7F2C8CF3272B`；`CMakeLists.txt` `29410BB2090AC08F7E80D96CA5CE1FD3FF34A47C125F56589F3856B35F4DDF0F`；`tests/cmake/NativeTests.cmake` `115D24827B8415B886437E675D196EE6558E424A8F13E803F1B15E805D0321AE`。

**范围限制**：这是真实 TCP/WebSocket 信令故障，不是 UDP/RTP 丢包、跨物理设备、真实网络设备断电、生产 TLS 或 8 小时稳定性。上述项目保持 `DEFERRED/NOT_RUN`；它们不影响这次限定场景 PASS，也不能据此宣称 Production Ready。当前没有本场景尚未修复的工程缺陷。
