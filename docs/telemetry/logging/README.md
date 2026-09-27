# 日志契约与实施证据

2026-09-27 更新：LG0 契约保持 `CONTRACT_FROZEN`，LG4～LG7 已有工程 PASS。当前入口为 [最终验收与延期项](evidence/FINAL_COMPLETION.md)，不再将本目录描述为仅 LG1 已实施。工程和运行结果各自绑定原输入，不自动覆盖当前未提交工作区。

- [event-catalog.json](event-catalog.json)：事件名、owner、级别、字段白名单、隐私规则、关联 ID 和预算；未知字段拒绝，语义/单位变更需升级版本。
- [migration-map.csv](migration-map.csv)：初始迁移清单；30 个文件、255 个文本匹配是当时静态扫描数量，不是当前运行事件量。
- [baseline.json](baseline.json)：LG0 原始 HEAD/输入指纹；保留历史身份，不更新为今天的哈希。
- [媒体端点身份契约](MEDIA_ENDPOINT_IDENTITY_CONTRACT.md)：消费绑定与网络对端身份分开。
- [支持包复盘手册](SUPPORT_RUNBOOK.md)：历史包、missing/unknown、符号和人工复盘边界。
- [同机双端限定 L3](evidence/l3-livekit-20260927/README.md)、[受控信令故障限定 L3](evidence/l3-network-20260927/README.md)。

`Room::Log/SetLogHandler` 与 `PanicCallback` 保持业务调用语义；原始错误留在业务/网络链，诊断使用受控安全摘要。不得因日志安全改写业务行为。

慢盘/断电、8 小时与 100 次生命周期、真实事故、Release 符号复盘、独立人工支持包审查、配对性能等仍以最终验收中的 DEFERRED/NOT_RUN 为准。旧日志不可追溯关联保持 missing，无证据异常终止保持 unknown。

历史阶段要求见 [总方案](../LOGGING_INDUSTRIALIZATION_PLAN.md)；实际选测遵守 [单一验证门策略](../../testing/TEST_STRATEGY.md)。
