# LG1 隐私与保存失败工程验收

状态：`ENGINEERING_PASS; L3_NOT_RUN`。日期：2026-09-26。源码 HEAD：`745af69d2ce2c154a17e72890a81784b21b4de67`；本阶段改动未提交。LG0 冻结输入及历史 checkpoint 见 [baseline.json](../../baseline.json)，该文件的前置哈希不随 LG1 改写。

## 变更与边界

- 聊天正文、显示名、附件名、传输失败原文不再进入会议控制台；诊断仅保留消息/传输类型、字节数与固定结果。真实聊天发送、接收、侧栏显示及失败提示保持原数据。
- 摄像头启动、切换和 DirectShow/WASAPI 诊断不再输出原始设备名称、路径、ID 或异常文本；切换失败的用户提示仍使用原始业务错误。其他身份及网络输出由 LG2/LG3 按迁移清单处理。
- 遥测历史只在原子写入成功后标记 `current_persisted_`。首次写入失败保留同会话 terminal 的重试机会；再次提交成功后不重复保存。自动退避与完整状态机留待 LG4。

## 实际门禁

- 复用 LG0 的 36 个无重复事件、30 个文件/255 个文本模式匹配的契约校验结论；LG0 未 build/CTest。LG1 后仅更新本阶段受影响输入。
- `cmake -S . -B build-debug`：PASS；Visual Studio 18 2026、Debug、Qt 5.15.18、Windows SDK 10.0.26100.0。未修改依赖，vcpkg 均为已安装。
- 首轮 `cmake --build build-debug --config Debug --target cohavora_app test_participant_window_remediation test_telemetry_report test_camera_switch_transaction --parallel`：PASS。首轮 focused CTest 中 `telemetry_report_test`、`camera_switch_transaction_test` PASS；`meeting_chat_log_privacy_test` 因测试专用精简窗口没有聊天侧栏而 FAIL，未将其记作产品失败。
- 修正 fixture 后，`cmake --build build-debug --config Debug --target cohavora_app test_participant_window_remediation test_camera_owner_remediation --parallel`：PASS。`ctest --test-dir build-debug -C Debug -R '^(meeting_chat_log_privacy_test|camera_owner_remediation_test)$' --output-on-failure`：2/2 PASS。先前两个测试的执行输入未变，结果复用；最终 focused 集合 4/4 PASS。
- `git diff --check`：PASS。编译中的 WebRTC `C4068`、`NOMINMAX C4005` 和链接 `LNK4217` 警告不影响此阶段结果；未把它们认作 LG1 源码失败。

## 可复核输入 SHA-256

```text
src/ui/meeting_room_window.cpp FF0EA629E6F7ECF0C5008C356BDA5FDFE7D8E701DBD8AC450218770327A78363
src/ui/meeting_room_window.h 2821A246330DACEB6839D6B548C9AC96921C9176611A4048D6F0DDBD98E76539
src/media/camera_source_manager.cpp 605B70164ECF7AE2A0237FB270E7D9E31E24DB52E053E53EC8DEB3E920DE55F2
src/media/dshow_capture.cpp 6D482869902FF6D0ACEF6659AE1335F12A3F8D7A78782D7F9605BA5F68910DB5
src/media/wasapi_capture.cpp B219BBE519ADE1166B0C8F49A0F510782A16C184F40E2E48468DA3F6C9E8EE9C
src/telemetry/telemetry_report.cpp E3BDAEE7942B3F8F7CB91AB8BDC51399FB347D3156090DCD4180C35AE510C044
tests/remediation/test_participant_snapshot_remediation.cpp 30538499C5EE3B5040A86F4C717B0883BBFB2B9EC1E380614E4F7FED49160071
tests/remediation/test_camera_owner_remediation.cpp 6F0190788108C1A9CEBCB1BCBED39917EDC342D7AAA31CB24C683D176DCC6EF7
tests/test_telemetry_report.cpp 7864AEB708F997E2BD2D6EAE564EAA94E30F4E1B8667E8A37EA5F8B27CDF1E00
tests/cmake/MeetingTests.cmake 8520C8628E47A95BB7F3BE87FC54D51F0B078065E6119BFBC80E0462F6B86479
tests/cmake/NativeTests.cmake AAB27EB4B91EC9B555DEEDB696D1CD8BA1FCAEB56031252304420AAD1F3588B5
tests/cmake/SecurityTests.cmake 18C15B8D7406239570AE62B640E2502D4C54A72430731CA6198A501E9B591F99
```

真实摄像头/麦克风、服务端、多用户、故障注入及长稳：`NOT_RUN/DEFERRED`。LG1 的工程通过不代表 LG2～LG7 完成或整个项目 Production Ready。
