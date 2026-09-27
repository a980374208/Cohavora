# LG3 工程验收证据

- 基线 HEAD：`745af69d2ce2c154a17e72890a81784b21b4de67`；工作区含先前 LG1/LG2 与本阶段未提交改动，未 commit/push。
- 构建目录：`build-debug`，Visual Studio 18 2026，Debug，`BUILD_TESTING=ON`，`COHAVORA_BUILD_QT_TESTS=ON`，`x64-windows-static`。MSBuild 在获准环境运行，以只读访问 Visual Studio/Windows SDK 定位信息。
- 最终构建：`cmake --build build-debug --config Debug --target cohavora_app test_diagnostic_pipeline test_openmeeting_http test_http_admission_owner test_camera_owner_remediation test_camera_switch_transaction test_local_unpublish_transaction test_unpublish_lifetime test_participant_snapshot_remediation test_participant_window_remediation test_meeting_startup_transaction --parallel`，退出码 0。
- 最终门禁：`ctest --test-dir build-debug -C Debug -L '^LOGGING_LG3$' --output-on-failure`，10/10 PASS。`git diff --check` 退出码 0；仅有 Git 对既有 LF/CRLF 工作区转换的提示。
- 修复记录：HTTP 请求传输成功而业务 DTO 解析失败时，原事件流误显示只有成功；新增同一 request ID 的 `http.response.decode_failed`。自有 participant、Coordinator、camera 旧输出因受限适配器会被抑制，改为固定原因码事件。Room 旧回调中的 identity、track SID/name 输出改为安全固定摘要；batch publish 与 unpublish 增加关联终态。
- 安全边界：测试覆盖旧 HTTP `operationID` header 不变、本地 UUID 唯一、普通隐私 canary、DTO 二次解析失败；新增/变更调用点扫描无活跃 `std::cout/cerr`、`qWarning`、`spdlog` 旁路。原始 SDP/ICE、token、聊天内容与设备路径未加入事件 DTO。
- L3：真实服务入会/断线恢复、硬件切换音视频质量、慢盘、断电、长稳均为 `DEFERRED/NOT_RUN`，未作为确定性测试 PASS 推断。
- 剩余边界：诊断窗口的启停审计在下一次活动检查时发生，到期后不会接受额外采样；主动 UI 入口在 LG6。CrashHandler 紧急路径在 LG5 处理。工程阶段关闭不代表 Production Ready。

## 最终输入指纹（Git blob）

| 文件 | SHA-1 |
|---|---|
| `src/telemetry/diagnostic_event.h` | `66de347adeb3c11e7cfd71b848ad65e1eacbe34f` |
| `src/telemetry/diagnostic_event.cpp` | `998c24f25030b57a6d10d9209c0004929aa2d814` |
| `src/telemetry/diagnostic_file_sink.cpp` | `2f191fa8a8953b94f62dd38452566ea3c0ca9cac` |
| `src/telemetry/diagnostic_pipeline.cpp` | `7fb9f23672b4063167434ce595cf670040b764db` |
| `src/core/room.cpp` | `ba55e5a66789168b81355d95d45c2e5d5988ec29` |
| `src/core/participant.cpp` | `51d2be081b1f14ca7935dfc1ba88facd12d01ef1` |
| `src/core/meeting_coordinator.cpp` | `d11c979c9c3c59394bb9b07ae47fc16fa102aca8` |
| `src/media/camera_source_manager.cpp` | `c86ca816c73d328039864998412d6b74aa66dd1b` |
| `src/net/openmeeting_http_client.cpp` | `7cbcda10b1d6f63a4e62ef5728912bc77968c9d5` |
| `src/rtc/webrtc_manager.cpp` | `11a302105fe03ed7fcce92d239013cdce520a84d` |
| `tests/test_openmeeting_http.cpp` | `cfaee142c598d6a3001e06bd3c5c0c18eceab47b` |
| `tests/CMakeLists.txt` | `16d542640416589321c176c36b596b46cacc7191` |
