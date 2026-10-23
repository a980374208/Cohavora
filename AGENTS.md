# AGENTS.md — LiveKit Native C++ / Qt

默认使用中文回答。保留无关用户改动，未经明确要求不 commit、push、发布、上传、reset、stash 或 checkout 用户工作。不自动修改 Reference、vendor、generated 或外部仓库。

## 项目与只读参考

目标：构建 LiveKit Native C++/Qt 客户端，保证 Room/Participant/Track 生命周期、Session 状态、reconnect、并发 ownership、Qt 同步和安全输出。

以下参考仓库只读，除非用户明确授权：

| 仓库 | 用途 |
|---|---|
| `E:\vsSource\WebRTC\client-sdk-cpp` | C++ API、对象模型、Rust FFI 与生命周期语义 |
| `E:\vsSource\WebRTC\client-sdk-flutter` | 客户端事件、UI 状态和 reconnect UX |
| `E:\vsSource\WebRTC\openmeeting` | Room/Meeting 业务、controller/view-model 与恢复流程 |

比较行为、语义和能力覆盖，不要求复制参考结构；C++ wrapper 不代表完整 Rust SDK 能力。

## 架构约束

- Session：`ASIO / Room / WebRTC callback -> Session Strand -> SessionRuntime -> immutable DTO / Qt queued signal -> UI`。会话可变状态只有一个逻辑 owner；销毁、替换或 generation 过期后拒绝 late callback。
- Startup：`Idle -> ConnectingRoom -> RoomConnected -> StartingLocalMedia -> InMeeting`。transaction 只有一个终态，失败必须 rollback；Qt 不持有 native resource lifetime。
- State：相关修改必须贯通 `LiveKit SDK -> native/business -> Qt/UI`；API、event 或 UI 单独存在不等于功能完整。
- Video：首选 `I420 -> bounded latest-frame router -> DX11`，回退 `I420 -> SIMD/libyuv -> QImage -> Qt`。GPU/CPU backend 互斥；callback thread 不访问 QWidget、D3D immediate context 或等待 UI/GPU；正常路径不做 GPU readback。
- Security：`raw detail -> business/network/state/callback` 与 `raw detail -> typed safe summary -> diagnostics` 分离。不得为日志安全改写业务语义；关注 URL、WebSocket、close/exception、SDP/ICE、Qt cache/clipboard、panic、proxy、region、refresh/reconnect。
- UI：默认复用 `AppTheme` 和当前统一控件规范；提示框、菜单和选择控件不得定义冲突样式或回退到原生尖角控件。

## 执行与构建

1. 读取 HEAD 和 `git status`，确认目标，只处理当前授权范围。
2. 配置输入和有效缓存未变时复用已有结果；仅分析变更触及的 caller/callee、public API、state、owner、thread、Qt projection、persistence 和 security sink。
3. 使用项目现有模式完成最小范围修改。先核对 staged diff；混合范围文件按 hunk 处理。
4. 只构建受影响目标；依赖或 CMake 输入变化时才重新 configure。失败、新差异或未解释风险才扩大范围。
5. 核对最终 diff/status，报告实际构建结果及限制。构建成功不代表真实服务、设备、网络或稳定性已验证。

Windows 的 Visual Studio/MSBuild 构建首次执行时直接申请获准环境，保持原命令不变，并说明 MSBuild 需要只读访问用户目录中的 Visual Studio/Windows SDK 定位信息；不要把沙箱访问或 SDK 定位失败当作源码编译失败。

使用 `CMakePresets.json` 中与当前工具链匹配的产品预设。多配置 generator 构建须明确指定 `--config`；性能与指标分析使用 `RelWithDebInfo`，Debug 仅作调试参考。不混用不同 CRT、Qt 或 WebRTC ABI。

修改 bug 后说明根因；有明确下一步建议时在完成回答末尾给出。

## 验收与报告

- 验收报告默认以 HTML 形式存放到 `samples\acceptance`。
- 后续更新验收文档统一遵循“按原格式，仅保留最新测试结果”。“原格式”指当前已推送版本的格式、布局、栏目及表格结构；除非用户另有明确要求，不重设计页面或增加重复的批次展示区。
- 按原测试主题及相同验收口径，仅用最新已完成测试的实测值、状态、日期和来源替换对应栏的历史数据；同一主题不重复展示已被替代的旧结果。不同子门分别保留范围与结论，不拼接为整组通过，不改变原门限、原始判定或授权例外边界。
- 未执行、仅尝试和未完成的测试暂不更新对应栏目，保留原已完成数据及其日期、来源；没有新完成结果时不以计划、准备或局部进度覆盖旧数据。
- 已被替代的旧报告及同名 JSON 移出当前验收目录，存入可追溯归档并维护引用；仍有独立范围未被新结果覆盖的报告继续保留。原始证据与审计资料保留，不随旧报告批量迁出。
- 后续验收测试计划统一维护于 `samples/acceptance/data/test-plan.json`。新增或调整测试时，优先合并到现有验收组及子门，避免另建重复清单或执行入口；批次汇总、历史结果与辅助诊断不重复计作产品验收项。
- 重测前先查私有测试导航和已有入口；同一范围重测只更换配置和新证据目录，不另建日期、批次或 `runXX` 包装脚本。优先给原入口补参数，公共能力复用已有模块；仅新增测量能力时增加模块，并登记回原验收组及导航。
- 所有可复用测试脚本、公共模块、配置模板和配套说明统一放到本机私有测试仓库 `..\Cohavora-private`或者'..\live-kit-test-local' 的合适位置：执行工具归 `tests/runtime/tools/<领域>/`，工具自测归 `tests/runtime/selftests/`，UIA 驱动归 `tests/uia/`，接入脚本及模板归 `integration/`，说明随对应工具的 README／领域导航维护；不得只保存在产品仓 `out/` 或临时目录。
- 使用现有私有 `integration/run_private.py` 及运行配置调用；上述绝对路径仅说明本机仓库位置，工具代码须从机器接入配置解析产品、私有和构建根，不硬编码本机路径。凭据和本机参数留在明确忽略的配置目录。历史冻结脚本副本和原始证据可留原位追溯，不作为后续重测的活动入口；工具自测、准备和诊断不另计产品验收项。
