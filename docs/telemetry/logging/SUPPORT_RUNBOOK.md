# 诊断包离线复盘

本流程只读取用户主动导出的 `cohavora-diagnostic-bundle-*` 目录。不上传、修改或重新打包源文件。包内默认没有内存 dump；`build.json` 的身份属于事故运行，不能用当前安装版本替代。

1. 在应用的遥测报告列表选择事故时间附近的历史会话，必要时指定 UTC 起止范围，再导出。记录所选会话、UTC 范围和导出结果。旧 v1 报告可导出，但其原始完整性、run/build 身份不可恢复；`legacy_unverified` 不是校验通过。
2. 先读 `manifest.json`。要求 `schema=cohavora-diagnostic-bundle`、`schema_version=1`，逐项检查 `files` 中相对路径、实际大小和 SHA-256。路径不得越出包目录；缺文件、hash 不符或不支持的未来版本应停止推断并保留原包。PowerShell 可对单个文件运行 `Get-FileHash -Algorithm SHA256 -LiteralPath <文件路径>`，把输出与对应 `files[].sha256` 比较。`manifest.json` 本身不是签名，文件 hash 只能检出包内文件的意外变化，不能证明来源可信。
3. 读 `manifest.json` 的 `missing`、`session_complete`、范围裁剪计数，再读 `diagnostics-health.json` 的时间线省略/坏行、checkpoint 缺 revision/裁剪及持久损失。任何缺口都要写进结论；不能把事件缺失解释成业务未发生。`sessions/<anonymous_session_id>/telemetry/checkpoint.json` 标识 v1/v2 和 `source_integrity`。
4. 用 `build.json` 的 `process_run_id`、EXE SHA-256 `build_id`、CodeView PDB GUID+Age `symbol_identity` 确定构建。先从受控归档找到 SHA-256 完全一致的 EXE，再核对 PDB 的 GUID+Age；没有匹配符号时只报告模块 RVA，不把相近版本 PDB 的行号当证据。发布版 EXE/DLL/PDB 归档和安装权限需在发布环境另验。
5. 按 `sessions/<id>/events.jsonl` 的 `process_run_id`、`anonymous_session_id`、`operation_id`、`parent_operation_id`、`request_id` 和 generation 串起 admission、HTTP、Room、媒体与重连。`event_sequence` 是进程内唯一序号，不等于因果顺序；跨设备只能用 UTC 粗对齐，进程内耗时用单调时钟。指标从 `telemetry/metrics.jsonl` 读取，不从日志文本重算。
6. 查 `sessions/<id>/stability.json` 的进程分类和崩溃元信息。只有 run/build 匹配且证据有效时才能称 `CONFIRMED_CRASH`；强杀、无证据退出和采集关闭保持 unknown/对应状态。元信息的主模块 RVA 需要匹配符号，包内无内存 dump，不能推断内存内容。

常见明确缺口：`historical_stability_session_link` 表示账本会话与遥测会话没有可信映射；`recovered_media_endpoint_identity` 表示能看出媒体恢复阶段，但不能认定具体端点；`operation_timeline`、`persistent_loss_summary` 或 `confirmed_crash_metadata` 缺失时分别无法还原操作、丢失范围或确认崩溃。复盘记录应逐项回答构建、run、会话、操作、失败阶段、结果与仍未知的证据。人工独立复盘和真实服务事故仍需外部条件，未执行时标 `DEFERRED/NOT_RUN`。
