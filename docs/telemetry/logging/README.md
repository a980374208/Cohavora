# 日志实施契约（LG0）

状态：`CONTRACT_FROZEN`。源码基线与输入 SHA-256 见 [baseline.json](baseline.json)。本目录是工业化日志方案的执行资料；事件目录和预算为拟实现契约，不表示代码已经实现。

- [event-catalog.json](event-catalog.json)：事件名、owner、级别、字段白名单、隐私规则、关联 ID 和初始预算。未知字段拒绝；修改字段语义或单位需要升级版本。
- [migration-map.csv](migration-map.csv)：自有 `src/` 的 30 个文件级迁移单元。`pattern_hits` 为同一正则的 255 个文本匹配（含定义），不是日志运行次数；实施时逐行审查实际调用点和 caller/callee。
- [baseline.json](baseline.json)：HEAD、工作区、源码/测试/CMake 指纹、可复用的历史证据范围与待测负载。未执行项保留 `NOT_RUN`。

LG1 已处理聊天正文、附件名和摄像头/麦克风原始设备标识，以及会话遥测历史的写入状态；工程验收见 [LG1 记录](evidence/lg1-20260926/README.md)。其他第一方输出仍按迁移清单在 LG2/LG3 接入统一管线；LG1 不宣称全项目日志均已完成隐私迁移。

`Room::Log/SetLogHandler`、`PanicCallback` 保持原调用语义。网络/业务继续使用原始错误；诊断只取受控摘要。HTTP 的 `operationID` header 暂不修改，新的本地 request ID 在 LG3 引入。现有 `SetHistoryEnabled(false)` 会清除已知遥测报告，LG1 保持该行为。

实现门禁见[总方案](../LOGGING_INDUSTRIALIZATION_PLAN.md)。LG0 为文档 Gate None；LG1 使用受影响产品构建和能覆盖隐私/保存风险的单一 L1 focused 测试集合。Windows MSBuild 首次构建按仓库 AGENTS.md 直接申请获准环境，不能把沙箱 SDK 定位失败判作源码失败。
