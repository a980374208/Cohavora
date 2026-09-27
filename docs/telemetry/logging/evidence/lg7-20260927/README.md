# LG7 工程验收与运行边界

日期：2026-09-27。结论：`LG7_ENGINEERING_COMPLETE; L3_DEFERRED`。HEAD `745af69d2ce2c154a17e72890a81784b21b4de67`，tracked dirty patch 指纹（`git diff --binary | git hash-object --stdin`）为 `ba8f0e34691b2b00228239f1b67ff0d715ba8e8c`；该指纹不含未跟踪源码，也不随被忽略的 `docs/` 变化。工作区保留 LG1～LG7 和用户原有未提交改动，无 commit、push、发布或 Reference 修改。

## 实际门禁与修复

- 配置：Windows 10.0.26200、28 逻辑处理器；`build-debug` Debug，Visual Studio 18 2026 / MSBuild 18.5.4，Windows SDK 10.0.26100.0。`CMAKE_HOME_DIRECTORY` 指向当前工作区。当前可用物理内存查询被系统拒绝，未填未经验证的本轮数值。
- 最终源码 `cmake --build build-debug --config Debug --target ALL_BUILD --parallel`：退出码 0。`ctest --test-dir build-debug -C Debug --output-on-failure`：87/87 PASS、0 failed、退出码 0，284.18 秒；其中 `LOGGING_LG7` 标签 4/4 PASS。完整结果可在 `build-debug/Testing/Temporary/LastTest.log` 核对。中途用户要求暂停时那次 CTest 在 57/87 后被主动中止，退出码 1，不计作 PASS；恢复后完整重跑。之后为补齐离会动作类别修改时间线，再在最终源码上完整重跑。
- `git diff --check`：退出码 0，仅有 Git 对现有 LF/CRLF 工作副本的提示。最终 probe：`build-debug/Debug/diagnostic_tryemit_probe.exe` 退出码 0；8 producer、每组 6000 事件、3 次配对，accepted 各 6000、dropped 各 0，TryEmit p99 为 15.7/22.3/25.9 us，最大 217.9/429.3/233.9 us。对照 wall time 0.2549/0.2454/0.2486 ms，开启 1.9933/2.0429/2.2223 ms。此为无真实媒体、无文件 writer 的合成入口探针，不推断 CPU、媒体回调、端到端延迟或真实运行结果。
- 发现并修复：typed 业务事件曾沿用旧字段名/事件前缀，导致 severity、component、HTTP `business_error`、渲染后端方向、订阅原因、重连模式和诊断窗口字段与 LG0 目录不符；对应序列化与控制台筛选已对齐。诊断包时间线原先丢掉已登记的 HTTP 状态、错误层及离会类别，后端离会失败无法仅从包定位操作和失败层；现保留安全的 `http_status`、`error_layer`、`leave_reason`，完整门禁通过。StabilityLedger 会话 ID 原先与 admission 匿名会话 ID 分别生成，历史包固定写入未关联；新会话共用经校验的匿名 ID，导出按 run/session 双重校验账本记录，旧会话仍标 missing。新增错误 run 同会话 ID 的负样例。
- 先前 manifest 原子替换失败后重试和普通日志留存设置恢复 fixture 已修复并由本轮完整套件覆盖。留存设置 fixture 初次失败根因是测试 INI 缺有效组织目录，修正测试隔离后通过；未改产品设置语义。旧失败和被中止测试不改记为 PASS。

## LOG-T01～T15

`PASS` 仅表示本机确定性部分；`DEFERRED`/`NOT_RUN` 独立保留真实运行部分。

| 场景 | 确定性结果与证据 | 外部运行结果 |
|---|---|---|
| T01 隐私 | PASS：聊天、姓名、文件/设备/token canary 的产品入口及安全 sink、UI、包回归；业务 payload 保真 | 真实多用户操作 DEFERRED |
| T02 Schema | PASS：非法 ID、未来版本、有限枚举及过长/坏行拒收；目录字段复核 | 无额外 L3 声明 |
| T03 满载 | PASS：`diagnostic_pipeline_test` 8 x 125000 = 1000000 次并发尝试；普通/关键区容量和丢失计数断言 | 长时间真实负载 DEFERRED |
| T04 UI | PASS：`diagnostic_qt_bridge_test` 停顿 10 秒时后台写入至少 3000 条，恢复展示 UI loss；通知合并/关闭竞争 fixture | 产品多 DPI/语言和实际阻塞 DEFERRED |
| T05 存储 | PASS：open/write/flush/manifest rename 故障注入、失败水位与恢复重试 | 慢盘/驱动故障 DEFERRED |
| T06 强杀 | PASS：隔离子进程提交非终态 checkpoint 后 `TerminateProcess`，重启读取 revision 1；账本不判 confirmed crash | 机器掉电耐久 NOT_RUN |
| T07 关停 | PASS：producer、writer、导出、会话停止竞争和唯一 callback 终态 | 真实慢 I/O 关停 DEFERRED |
| T08 兼容 | PASS：v1 只读安全导出、v2 SHA-256、未来版本保留、缺失/截断/坏 hash 区分 | 无额外 L3 声明 |
| T09 HTTP | PASS：同毫秒 request 唯一性、parent、取消/迟到、错误层及 legacy wire header 回归 | 真实后端协议联调 DEFERRED |
| T10 重连 | PASS：resume 转 full、终态/迟到与安全时间线回归；日志丢失不改变业务分母 | 真实网络故障和多设备 DEFERRED |
| T11 Crash | PASS：隔离访问异常、abort、terminate、强杀、provider 缺失/损坏/错误 build；Debug PDB GUID+Age 和 RVA 精确匹配 | 真实事故与 Release 符号归档 DEFERRED；内存 dump NOT_RUN（默认禁用） |
| T12 Bundle | PASS：重启选旧报告、读租约/轮转竞争、取消、超限、恶意 ID、逐文件 hash、默认无 dump | 产品 UI 人工演练 DEFERRED |
| T13 配额 | PASS：多 run、活跃租约、临时/坏件、孤儿段和有界重试；正常日志 100 MiB/7 天、分段 10 MiB | 慢盘和长期真实写入 DEFERRED |
| T14 支持复盘 | PARTIAL：一个合成安全包可读构建、入会失败、重连降级、普通离会动作和后端通知 HTTP 503/错误层及显式缺口；旧记录缺可信 session 关联仍标 missing | 三份真实产品包及另一名人工审阅者 DEFERRED |
| T15 留存偏好 | PASS：telemetry 历史关闭/清除与普通日志关闭/清除彼此独立，设置重启恢复 | 真实用户设置演练 DEFERRED |

## 冻结预算与覆盖

预算沿用计划 11.2，未为取得 PASS 放宽：非实时 TryEmit p99 <=100 us、实时回调增幅 <=5%、CPU <=2 个百分点、额外稳态工作集 <=64 MiB、UI 每批 <=4 ms 且 <=20 Hz、正常日志量 <=100 KiB/分钟、8 小时会议及 100 次生命周期。仅上述合成 TryEmit 三组达到其候选阈值。其余需要相同构建、真实媒体负载、配对对照和采样口径，全部 `DEFERRED/NOT_RUN`；静态硬界和 10 秒 UI 停顿测试不是这些性能实测。

| 范围 | 覆盖状态 | 依据与限制 |
|---|---|---|
| LG0 字段/隐私契约，LG1 止漏 | IMPLEMENTED + VERIFIED_DETERMINISTIC | 有限事件/字段、源头摘要、canary 测试；冻结 `event-catalog.json` 中的 `implementation_status` 是 LG0 当时基线元数据，不作为当前交付状态 |
| LG2 有界管线/存储 | IMPLEMENTED + VERIFIED_DETERMINISTIC | 8192 条/8 MiB 入口、1024 条/1 MiB 关键预留、16 KiB 事件、10 MiB 轮转、100 MiB/7 天配额；丢弃/故障计数 |
| LG3 关联/业务事件 | IMPLEMENTED + VERIFIED_DETERMINISTIC | run/session/operation/request、legacy HTTP header、重连/媒体安全事件；真网 DEFERRED |
| LG4 checkpoint/恢复 | IMPLEMENTED + VERIFIED_DETERMINISTIC | 15 秒非终态提交、原子 manifest、SHA-256、有限重试、隔离强杀；断电 NOT_RUN |
| LG5 crash | IMPLEMENTED + VERIFIED_DETERMINISTIC | Windows 固定元信息 provider、run/build 匹配与未知终止分类；Release 符号和实际事故 DEFERRED，内存 dump UNSUPPORTED（本版策略关闭） |
| LG6 历史安全包/控制台 | IMPLEMENTED + VERIFIED_DETERMINISTIC | v1/v2 历史选择、窗口/过滤、读租约与 hash、run/session 账本；人工独立复盘 DEFERRED |
| LG7 L2 与本机故障矩阵 | VERIFIED_DETERMINISTIC | ALL_BUILD、87/87、T01～T15 的本机部分；真实运行列为 DEFERRED/NOT_RUN，未获得 VERIFIED_RUNTIME |

兼容输出审查：`Room::Log/SetLogHandler` 仍通过受控安全摘要维持旧消费回调合同，UI `legacy` 通道不写入第二个普通文件 sink；未知自由文本在队列前抑制。`telemetry.h` 的旧 `std::cout` 指标类只有 CMake 列为头文件，当前产品/测试无使用点，不形成实际重复输出。`CrashHandler::FlushLogs` 仅在显式 `TriggerPanic` 路径，未在 signal/SEH handler 内执行。未发现可安全删除且已到期的运行中兼容例外；不为清理而改变旧 callback 次数、线程或业务语义。

支持操作：在历史列表选事故报告并设 UTC 范围，导出后先核 `manifest.json` 的 source_format、missing、loss 和逐文件 SHA-256，再看 `build.json` 的 build/symbol、会话 `events.jsonl` 的 operation/request/stage/outcome、`stability.json` 的 run/session/crash 分类。v1 的 run/build 与源完整性不可恢复；重连后的具体媒体端点身份仍在 `missing`，不可从匿名事件猜测。元信息 provider 默认不含 dump，用户主动导出也不自动上传。Release EXE/DLL/PDB 归档、安装目录 ACL 与实机符号复盘均需实际发布环境。

关键最终 SHA-256（其余阶段输入见 LG4～LG6 证据）：`stability_ledger.cpp` `A584C7D0F5B44B2D7CD47E9B6148EE2091F3C9656BF36D8B159702DE18A26DD8`；`stability_ledger.h` `4D47ECC2871AFEFC0FB51DC53ABB88F9B424409EC1B495FABE0425062E68E131`；`diagnostic_bundle.cpp` `E48D3B01A5C8870E19DEA4E2CAFD55E60F115AC3979C34B9FB8CEEEF6B9746A4`；`telemetry_operation_timeline.cpp` `77E1172EBBCA656BB68B5B2CD30D40995C7B6D450A37B7AC19FFD6FE99A5842E`；`telemetry_operation_timeline.h` `862F5B168782B75D906CAB37A78139BE2EFCAB6CC294AA192728903F7AE9BFF9`；`meeting_coordinator.cpp` `D9EF5AD8253C92C7E298D5E0BE0C01B80D3F97F18D3D8A4EB6CC4C92C6237D53`；`test_stability_ledger.cpp` `BD9BE99B827279AE9089B84931D1A342E7D3608DDE851D99E1DDC208262152CA`；`test_telemetry_report.cpp` `E19AD02B65626E5F9FA5F5ADECE3240D97DF6CEEC5D711A1EAA232CB181148FC`；`CMakeLists.txt` `29410BB2090AC08F7E80D96CA5CE1FD3FF34A47C125F56589F3856B35F4DDF0F`；`NativeTests.cmake` `115D24827B8415B886437E675D196EE6558E424A8F13E803F1B15E805D0321AE`。

本阶段退出条件：本机可执行实现及 L2 PASS；真实服务、多设备/真网、慢盘、断电、8 小时会议/100 次循环、发布符号、三份产品支持包和独立人工复盘保持外部矩阵。未把 `NOT_RUN` 写成 PASS，也未声明 Production Ready。
