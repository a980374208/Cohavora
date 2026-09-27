# LG4 阶段证据

- 基线 HEAD：`745af69d2ce2c154a17e72890a81784b21b4de67`。保留 LG1～LG3 与 LG4 工作区修改；本轮没有 commit/push，也没有修改 Reference。
- v2 checkpoint 先提交 SHA-256 分段，再原子替换 manifest；Windows 独占 writer lock。已增加单报告滚动、`pruned_records/pruned_segments`、孤儿段清理和 8 MiB 本应用坏件隔离预算；未知未来版本保留，v1 只读。活跃 pending 报告即使初始时间超过保留期也不会被清理。
- `TelemetryHistoryStore` 按会话 ID 保留最多 4 个 pending 报告、8 MiB 待写预算；排队的 Snapshot 已换为安全 JSONL checkpoint 和固定元数据，默认最多 64 jobs/16 MiB，并在字节或条数满时优先让非终态任务退出。队列深度和字节数可查询。控制任务 callback 捕获的间接内存尚未建立严格计费，不能把这项单独宣称为整个 store 的内存硬界。15 秒 checkpoint、终态优先、1/5/30/60 秒退避和手动重试已接入。源时刻合桶，admission 匿名会话 ID 与诊断管线 `process_run_id` 贯通到新建 v2 manifest；旧 v2 缺失 run ID 保持 unknown，跨 run 追加被拒绝。
- 从 LG3 typed JSONL 读取最多 1024 条操作时间线；只接受已知事件、同目录 run ID、目标会话与受控操作 ID，拒绝过长/半行和篡改 ID。修复了诊断事件原先拒绝 `connect:42:1` 操作 ID 的根因。导出同步拒绝由 callback 唯一报告，UI 不再重复弹框。
- 直接验证：Visual Studio 18 / Windows SDK 10.0.26100.0 / `build-debug` Debug，`cohavora_app`、`test_diagnostic_pipeline`、`test_telemetry_report` 构建退出码 0。此前 LG4 标签 2/2 PASS（21.42 秒）；时间线安全校验经直接测试 1/1 PASS（2.07 秒）；队列和固定指标目录经直接测试 1/1 PASS（22.13 秒）；最新 run ID 修改经产品及 `test_telemetry_report` 构建、直接测试 1/1 PASS（22.33 秒）。LG4 最终 focused gate 尚未执行。`git diff --check` 退出码 0；只出现现有换行格式提示。最终源码 SHA-256：`telemetry_operation_timeline.cpp` `E5184132F0A10CA7140DBF982601B425544B00DC7CCF6BF3FB43E6AD2E40C8FE`，`telemetry_checkpoint.cpp` `27B706BB3552BE148B155D42D6EA808B47E7D199CB8DB22F4410341BBC962B3E`，`telemetry_report.cpp` `BA24E48F4A567EE2DBC28F02A2D67771202A738EBF194A02FF3E77FD347618A2`，`session_telemetry.cpp` `8F5D7972FF05760A369458EFA2564F7F67D0ED4F56EBD6C82FFFD05ECA6470FB`；直接测试 `test_telemetry_report.cpp` `72701F39D6A0F51AB1E9CC766D10D1CED0DCB8D4060C9AC3C07CD6B2E47A2345`。
- 本轮测试新增：滚动/孤儿/未来版本/坏件预算、单次控制回调、手动写盘失败后恢复、源会话 ID、活跃报告零保留期、时间线过滤/上限/损坏行、队列字节界先于 64 jobs 触发、普通 canary 不能进入动态产品链指标键及 checkpoint、v2 run ID 读取和跨 run 拒写。此前修过新目录链接误报、失败状态被内存快照覆盖、终态 revision 重复。
- **仍为 `IN_PROGRESS`**：控制任务间接内存与 Session Strand 同步序列化成本需要核对；pending 丢失范围持久摘要、总 100 MiB/7 天配额对临时/坏件及并发 run 的协调、读租约和导出与轮转竞争、准确 build ID 与时间线历史包集成、LG4 最终 focused L1 gate。StabilityLedger 的 run ID 尚未与诊断管线对齐，归 LG5 处理。当前直接测试 PASS 不代表 LG4 阶段 PASS。
- L3：真实慢盘、断电、真实服务/设备及长稳为 `DEFERRED`；断电耐久性为 `NOT_RUN`。不从 Build/CTest 推断运行验收。

## 关机恢复后增量（仍未关闭）

- 基线 HEAD 仍为 `745af69d2ce2c154a17e72890a81784b21b4de67`。本轮保留所有已有未提交改动，不 commit/push；另有非本轮修改出现在 `tests/runtime/`，未触碰。
- v2 增加根目录跨进程独占锁、单报告读租约和运行期锁；删除操作全程持独占锁，扫描遇到正在写入的报告延后判断，活动 run 的未完成报告不被另一进程回收。源 revision 跳号写入 manifest 有界缺失范围；队列拒绝和 pending 淘汰另写有界 `losses-v1.json`，失败保留有限重试状态。总预算写前计入本应用 v1/v2 已知段、临时段、坏件及丢失摘要；历史清理计算不再只看有效报告。
- `SubmitSnapshot` 的复制与 JSONL 投影移出状态互斥锁，另以提交锁维持入队顺序；它仍在调用线程同步做投影，Session Strand 耗时目标尚需评估。控制回调捕获的间接内存尚未硬计费。
- 历史 v2 报告可按索引 ID 导出安全包；持读租约重建允许的数值指标、受控操作事件、真实 EXE SHA-256 build ID、缺失/截断摘要及逐文件 SHA-256 清单。普通文本值不进入包；默认无 dump。UI 报告列表导出选中历史报告，保留现有当前报告入口。导出限制为一个执行、一个等待，第三个请求回调拒绝。
- 直接验证：`cohavora_app test_telemetry_report` 构建退出码 0（CMake 因新增源码正常再生成）；之后修改了导出名额归还时机，产品 target 需在最终 gate 重建。最新 `test_telemetry_report` 构建退出码 0，`telemetry_report_test` 1/1 PASS（23.13 秒）。`git diff --check` 退出码 0，只有既有换行提示。曾出现的目录条目数假设、Windows 缺失锁/导出目录误判、导出名额提前归还及测试 JSON 类型比较失败均已修复并重测；失败不改记为历史 PASS。
- 当前输入 SHA-256：`telemetry_checkpoint.cpp` `FDE559E26960698E03506233E1BBD76DA5F39E8F4ADBB5BBA3D35574193C335A`；`telemetry_report.cpp` `BAD647C5BD7E2F5A9D052829AA0C4540539CB4CF16330F862211B38000D48776`；`diagnostic_bundle.cpp` `088EF1B69968572FBD511CC89E6488E02D0661E67AFD35BB36E8B86F16803E94`；`build_identity.cpp` `B888CB4528EA83BA4D566AE5CCEB314F49307D546CBFE77AEC3D090B1D867C0C`；`telemetry_operation_timeline.cpp` `D7CF6254EF99729BE330BB73F718E37891107583FFC1B8A4C215B6973D7347C2`；`test_telemetry_report.cpp` `B1B699C6B25840148F81BC0BE79781011DB7FAE9845F33F0E10DBD1924032744`。
- **仍为 `IN_PROGRESS`**：控制回调及保留对象间接内存硬界、Session Strand 同步投影成本、旧 v1 临时目录与根级遗留锁清理、导出与轮转的更多失败/并发矩阵、LG4 focused L1 产品门禁。历史 v1 目前保持只读索引，但安全包仅支持 v2；不可把 v1 安全导出写成 PASS。LG5～LG7 尚未实施。真实慢盘、断电、真实服务/设备及长稳继续 `DEFERRED/NOT_RUN`。

## LG4 工程验收（2026-09-27，取代上方进行中状态）

- 结果：`LG4_ENGINEERING_COMPLETE`。Snapshot 入口只提交不可变对象与源时间，安全投影由 worker 完成；待处理及在途 job 同时受 64 项/16 MiB 预算约束，内存趋势受 300 桶/8 MiB 双界。控制回调使用 256 字节内联闭包；产品捕获仅含非拥有的 Qt guard、受控报告 ID 或固定取消状态，导出另限制一个执行、一个等待。这里的预算限定本 store 拥有的队列和产品调用路径，不声称任意外部调用者共享对象的间接堆内存也由 store 计费。
- 旧 v1 临时目录的确认归属文件计入总预算，过期后只清理本应用文件。根级跨进程锁、运行期锁和报告读租约协调写入、轮转、删除及历史导出。v2 manifest 持久化缺失 revision/源时段；队列拒绝和 pending 淘汰写有界 `losses-v1.json`，清历史同步删除它。旧 v1 只读索引保留，安全包导出目前仅支持 v2，LG6 处理旧版支持流程。
- 历史 v2 安全包经索引选择、持读租约重建数值指标和受控事件，包含 EXE SHA-256 build ID、缺失说明、逐文件 SHA-256，默认不含 dump。UI 可导出选中历史报告。会话切换、终态重试、停止排空、取消和导出名额由直接测试覆盖。
- 本轮发现并修复配额清理根因：归属文件统计失败曾被当作已满而可能误删旧报告；成功删除后也曾用索引大小估算剩余占用，在并发 run 下可能偏离实际。现在计数失败停止清理并标 `met_06_history_scan_failed`，删除后重新读取实际归属字节。新增损坏摘要路径下保留有效报告的回归测试。
- 实际 gate：Windows Visual Studio 18 / SDK 10.0.26100.0，`build-debug` Debug；最新源码 `cmake --build build-debug --config Debug --target cohavora_app test_telemetry_report --parallel` PASS，`test_diagnostic_pipeline` 重建 PASS；`ctest --test-dir build-debug -C Debug -L '^LOGGING_LG4$' --output-on-failure` 2/2 PASS，26.63 秒。修复迭代的 `telemetry_report_test` 1/1 PASS，25.43 秒。`git diff --check` 退出码 0，仅有换行提示。此前失败与旧 PASS 保留在上方，不重新标记。
- 最新输入 SHA-256：`telemetry_report.cpp` `BF154B2A10C4DE490CE7134A5B82C2CADD0736BB817096217F8343F904B4A83D`；`telemetry_checkpoint.cpp` `64EA19F25E51EE989D5DD842DBB6FEAC00F65D2932BCAE62EADEB530219534F3`；`bounded_callback.h` `A1BE746CB83FC4F6B11A2440440AE56D6AE0237200E7869C465FDEF89B0C9524`；`diagnostic_bundle.cpp` `088EF1B69968572FBD511CC89E6488E02D0661E67AFD35BB36E8B86F16803E94`；`test_telemetry_report.cpp` `BDEE1C4D8280D12F28D4507807652E01796B2683151BABAFD1739D228F612209`；`CMakeLists.txt` `AFFBA0FCE154757B5ABA258B923489CD492EC756A7B12CAF2BD9AC0E8C3C5462`；`NativeTests.cmake` `561CE23A78278D7C284E5A029BEDC35785620F84B9D28D26AEE7360CA04D936D`。
- 外部验收：真实慢盘、多进程持续负载、断电耐久性、真实服务/设备和长稳 `DEFERRED/NOT_RUN`；未把 Build/CTest 推断为 L3 PASS。LG5 crash 与账本关联、LG6 旧 v1 工作流、LG7 支持复盘继续按计划执行。
