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

## 2. 不可破坏的架构约束

- Session：`ASIO / Room / WebRTC callback -> Session Strand -> SessionRuntime -> immutable DTO / Qt queued signal -> UI`。会话可变状态只有一个逻辑 owner；销毁、替换或 generation 过期后拒绝 late callback。
- Startup：`Idle -> ConnectingRoom -> RoomConnected -> StartingLocalMedia -> InMeeting`。transaction 只有一个终态，失败必须 rollback；Qt 不持有 native resource lifetime。
- State：相关修改必须贯通 `LiveKit SDK -> native/business -> Qt/UI`；API、event 或 UI 单独存在不等于功能完整。
- Video：首选 `I420 -> bounded latest-frame router -> DX11`，回退 `I420 -> SIMD/libyuv -> QImage -> Qt`。GPU/CPU backend 互斥；callback thread 不访问 QWidget、D3D immediate context 或等待 UI/GPU；正常路径不做 GPU readback。
- Security：`raw detail -> business/network/state/callback` 与 `raw detail -> typed safe summary -> diagnostics` 分离。不得为日志安全改写业务语义；关注 URL、WebSocket、close/exception、SDP/ICE、Qt cache/clipboard、panic、proxy、region、refresh/reconnect。
- UI：新增或修改界面未明确指定视觉样式时，默认复用 `AppTheme` 和当前统一控件规范；提示框、菜单和选择控件不得定义冲突样式或回退到原生尖角控件。
