# AGENTS.md — LiveKit Native C++ / Qt

本文件只补充 LiveKit 项目规则；通用语言、Git、文档和最小验证原则继承全局 AGENTS.md，不重复声明。

## 1. 项目与只读参考

目标：构建 production-grade LiveKit Native C++/Qt 客户端，保证 Room/Participant/Track 生命周期、Session 状态、reconnect、并发 ownership、Qt 同步、安全输出和证据可追溯。

以下参考仓库只读，除非用户明确授权：

| 仓库 | 用途 |
|---|---|
| `E:\vsSource\WebRTC\client-sdk-cpp` | C++ API、对象模型、Rust FFI 与生命周期语义 |
| `E:\vsSource\WebRTC\client-sdk-flutter` | 客户端事件、UI 状态和 reconnect UX |
| `E:\vsSource\WebRTC\openmeeting` | Room/Meeting 业务、controller/view-model 与恢复流程 |

比较行为、语义和能力覆盖，不要求复制参考结构；C++ wrapper 不代表完整 Rust SDK 能力。

### 文档存放约定

- 项目自有的 Markdown 文档（`*.md`）除 `AGENTS.md` 和各级 `README.md` 外，一律存放在 `docs/` 下，不得放在源码、测试或仓库根目录。
- 优先使用与主题对应的子目录，例如白板相关文档放在 `docs/whiteboard/`；已有明确领域目录时沿用现有目录，不重复创建近义分类。
- 新建或恢复计划、实施方案、设计方案、路线图、检查点、验收记录和分析报告时，先确定 `docs/` 下的领域目录，再创建文件。
- 移动文档后同步更新项目内引用；第三方、vendor、generated 和外部仓库自带的 Markdown 保持原位，不因本规则改动。
- 待验收项统一从 `docs/README.md` 入口维护：状态变化时更新该入口及其链接的对应领域验收记录，不另建重复状态清单；新增证据链接到已有条目，并保留历史版本、失败结果和未执行边界。

### Runtime 工具存放约定

- 后续新增 runtime 工具必须直接归入 `tests/runtime/tools/` 下对应领域目录：`product_acceptance/`、`meeting/`、`screen_capture/`、`desktop/`、`media/` 或 `diagnostics/`，不得平铺到 `tests/runtime/` 或 `tools/` 根目录。
- 领域职责以 `tests/runtime/tools/README.md` 为准；优先复用已有分类，确无适用领域时再新增目录并同步更新索引。配套配置、worker 和同领域模块随工具放置，跨领域依赖显式声明。
- 工具离线自测放 `tests/runtime/selftests/`，真实桌面自测放 `tests/runtime/desktop_checks/`；保留已有探针、编排和独立工程边界，不因目录整理改变执行条件或 CTest 注册语义。

## 2. 不可破坏的架构约束

- Session：`ASIO / Room / WebRTC callback -> Session Strand -> SessionRuntime -> immutable DTO / Qt queued signal -> UI`。会话可变状态只有一个逻辑 owner；销毁、替换或 generation 过期后拒绝 late callback。
- Startup：`Idle -> ConnectingRoom -> RoomConnected -> StartingLocalMedia -> InMeeting`。transaction 只有一个终态，失败必须 rollback；Qt 不持有 native resource lifetime。
- State：相关修改必须贯通 `LiveKit SDK -> native/business -> Qt/UI`；API、event 或 UI 单独存在不等于功能完整。
- Video：首选 `I420 -> bounded latest-frame router -> DX11`，回退 `I420 -> SIMD/libyuv -> QImage -> Qt`。GPU/CPU backend 互斥；callback thread 不访问 QWidget、D3D immediate context 或等待 UI/GPU；正常路径不做 GPU readback。
- Security：`raw detail -> business/network/state/callback` 与 `raw detail -> typed safe summary -> diagnostics` 分离。不得为日志安全改写业务语义；关注 URL、WebSocket、close/exception、SDP/ICE、Qt cache/clipboard、panic、proxy、region、refresh/reconnect。
- UI：新增或修改界面未明确指定视觉样式时，默认复用 `AppTheme` 和当前统一控件规范；提示框、菜单和选择控件不得定义冲突样式或回退到原生尖角控件。

## 3. 单一执行流程

每个任务只走一次以下流程：

1. **定界**：读取 HEAD、`git status` 和一个最近相关 checkpoint；用户已给定可信 checkpoint 时直接使用，不搜索替代版本。
2. **复用**：比较相关源码、测试、CMake 和证据哈希。执行输入未变则复用 PASS，跳到第 6 步。
3. **影响分析**：只追踪变更触及的 caller/callee、public API、state、owner、thread、Qt projection、persistence 和 security sink；不扫描无关模块或全部 Reference。
4. **执行当前阶段**：audit、design、implementation、review、capability、regression 或 runtime validation 只完成用户要求的阶段；audit 不自动修复。
5. **选择一个验证门**：按第 4 节选择能覆盖风险的最小单一 gate，不顺序累加多个 gate。
6. **收口**：检查最终 diff/status，简短记录实际验证、复用/跳过理由和剩余风险，然后停止。

失败、新差异或未解释风险才回到第 3 步扩大范围。不得因“更放心”重复已经通过且输入未变的步骤。

## 4. 验证门

| Gate | 触发条件 | 唯一最终动作 |
|---|---|---|
| None | 文档、报告、commit message、staging、提交边界；或有效 checkpoint 哈希完全一致 | 必要内容/格式/哈希检查；不 build、不跑 CTest |
| Metadata | 仅 CMake test label/CTest metadata 变化 | configure 或 discovery 检查；不执行测试，除非执行语义改变 |
| L0 | 局部实现、单模块、低 blast radius | `git diff --check` + affected target build + 最接近的 1～3 个测试 |
| L1 | P1、共享核心边界、跨 thread/owner/state/reconnect/security | affected/core consumer build + `CORE_REGRESSION` **或**能完整覆盖的更窄 focused 集合 |
| L2 | P0、结构性 Room/Coordinator/SessionRuntime/threading/reconnect/media 修改、多个关联 P1、Snapshot/RC/Release gate | current-source `ALL_BUILD` + 一次 Full CTest |
| L3 | 真实服务、设备、网络、多用户、长稳 | 只执行与声明直接相关的 runtime matrix |

去重规则：

- 禁止 `targeted -> focused -> security -> core -> full` 全链路。
- 较大 gate 覆盖较小 gate 时，只保留较大 gate；L2 最终不再单跑 L1。
- 迭代只跑失败或直接受影响测试，稳定后执行一次所需最终 gate。
- L1 的直接测试已在 CORE 中时不拆跑；只有定位失败或必须保留独立证据时例外。
- 局部变更不跑 `ALL_BUILD`；CMake/依赖未变且 build tree 有效时不 configure。
- 测试源码变化先运行该 target，再由实际影响决定是否升级；不新增语义重复测试。
- L3 未执行只能标 `NOT_RUN/DEFERRED/UNKNOWN`；Build/CTest/Static PASS 不等于 Production Ready。

## 5. Audit、Capability 与证据

- 优先 incremental diff；Reference HEAD 未变则复用 baseline。
- P0/P1 需要具体源码证据与可复现或边界明确的推理。
- 仅在受影响时追踪 Connect、Disconnect、Publish/Unpublish、Subscribe/Unsubscribe、Participant/Track lifecycle、Camera/Microphone、Reconnect。
- 区分 implementation difference、capability gap、behavior difference、optimization 和 verified defect。
- Capability 链：`Rust -> client-sdk-cpp FFI -> target native -> Qt/business -> UI`；状态使用 `IMPLEMENTED/PARTIAL/MISSING/WRONG_BEHAVIOR/OPTIMIZATION_REQUIRED/UNKNOWN`。
- 最新 checkpoint 和状态迁移是权威来源；不在本文件复制易过期的 finding 状态。
- committed 与 uncommitted delta 分开；保留旧失败、fingerprint 和 verdict，不把未执行项改写为 PASS。

## 6. 完成边界

说明最终行为、验证选择及限制。工程关闭不自动等于整个项目 Production Ready；用户限定阶段时完成后即停止。
