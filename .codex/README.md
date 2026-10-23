# 项目级 Codex agents

当前包含适配自 [wshobson/agents](https://github.com/wshobson/agents) 的三个基础角色，以及本项目定制的 `qt_reviewer`，供 C++20/Qt/WebRTC 项目按任务调用。仅维护这四个角色，不引入上游整套插件、hooks、MCP、依赖或全局配置。

## 角色与调用

| 名称 | 默认职责 | 写入边界 |
|---|---|---|
| `cpp-pro` | RAII、生命周期、并发 ownership、C++20/Qt/WebRTC 的局部实现 | 仅执行当前用户授权内、主 agent 明确委派的实现 |
| `code-reviewer` | 独立检查指定 diff 和必要调用链，返回有证据的可操作问题 | 默认只读 |
| `debugger` | 依据已有源码、日志和堆栈区分症状、候选原因与根因 | 默认只读；修复交给主 agent 或 `cpp-pro` |
| `qt_reviewer` | 检查 UI 线程、QObject 生命周期、主题/控件、DPI、状态投影与 Qt/原生渲染交接 | 默认只读；动态验证由主 agent 调度 |

在以本仓库为工作目录的新会话中，可以直接使用自然语言指定角色，例如：

```text
使用 code-reviewer 审查本次指定 diff，只读检查生命周期、并发与 Qt 状态贯通。
使用 debugger 分析这个退出堆栈，先给出有证据的根因和一个最小验证建议。
使用 qt_reviewer 只读审查指定界面改动，区分源码确认问题和视觉待验证项。
使用 cpp-pro 实现已经确认的修复；由主 agent 统一安排一次最小验证 gate。
```

这四个 TOML 文件按 [Codex 自定义 agent 格式](https://developers.openai.com/codex/multi-agent#custom-agents) 放在 `.codex/agents/`，名称以文件内的 `name` 为准。已运行会话是否刷新发现由客户端决定；本次不修改并发上限、模型或全局配置。四个角色均省略模型和推理强度，继承父会话的配置。

角色中的“只读”是行为约束。工具、沙箱和审批权限继承父会话；这些文件不创建独立的只读沙箱。由主 agent 协调单一写入者、同一构建目录的唯一执行者，以及当前用户授权内的验证。

普通改动由 `code-reviewer` 主审；Qt 专项改动由 `qt_reviewer` 主审，按明确范围追踪必要调用链。主 agent 合并去重结果，不对同一 diff 再安排完整重复审查。构建与测试统一由主 agent 或指定的唯一执行者完成。

## 项目适配

- 优先遵循用户当前指令、根目录及作用域内的 `AGENTS.md`，不引入替代工作流。
- 增加 Session Strand 单 owner、generation/late callback、Startup transaction/rollback、SDK 到 Qt 状态贯通、视频 callback 和安全输出边界。
- 实现保持 C++20 与现有 MSVC/Qt/WebRTC ABI；UI 复用 AppTheme。删除上游模型别名，避免为采用新特性升级标准、依赖或工具链。
- 删除与当前任务无关的自动工具安装、全面扫描、Linux 专用工具默认调用和通用“全部测试通过”要求。只按项目规则选择一个最小 gate，复用有效结论，由主 agent 统一调度。
- 首次 MSBuild 构建按项目规则直接申请获准环境。测试入口从当前 presets、缓存及适用的本机私有接入发现，不假设公共 `tests/` 存在，不把本机私有路径或凭据写进通用配置。
- 默认只读审查和诊断不触发程序运行、设备/网络测试或 trace 抓取；已授权且明确委派的验证按原范围执行。Build/Unit/Static PASS 不提升为 runtime 或 Production Ready。

## 来源

`qt_reviewer` 于 2026-10-17 根据本项目规则定制，沿用本地审查角色的协作及证据约定，没有独立的上游 Qt agent 文件。以下固定提交、上游角色及原始文本哈希对应原有三个基础角色和上游许可。

适配日期：2026-10-16。固定上游提交：[044b4b21f083948e69ec878a7b07c8d0951a6ffa](https://github.com/wshobson/agents/commit/044b4b21f083948e69ec878a7b07c8d0951a6ffa)。不自动跟随上游更新；后续扩充按具体任务选择角色。

| 本地角色 | 上游文件 | 上游原始名称 |
|---|---|---|
| `cpp-pro` | [systems-programming/agents/cpp-pro.md](https://github.com/wshobson/agents/blob/044b4b21f083948e69ec878a7b07c8d0951a6ffa/plugins/systems-programming/agents/cpp-pro.md) | `cpp-pro` |
| `code-reviewer` | [comprehensive-review/agents/code-reviewer.md](https://github.com/wshobson/agents/blob/044b4b21f083948e69ec878a7b07c8d0951a6ffa/plugins/comprehensive-review/agents/code-reviewer.md) | `comprehensive-review-code-reviewer` |
| `debugger` | [debugging-toolkit/agents/debugger.md](https://github.com/wshobson/agents/blob/044b4b21f083948e69ec878a7b07c8d0951a6ffa/plugins/debugging-toolkit/agents/debugger.md) | `debugging-toolkit-debugger` |

固定提交中原始 UTF-8 文本的 SHA-256（用于核对来源，不代表本地改写后的文件）：

```text
cpp-pro.md       0fe221390dce11b69caa128475d305f0dbff8854bcfaaaa19bcca5fa74c29b10
code-reviewer.md 1d4be63448f48c10956e4395e54c91fde6874dca26567944aaf657acc7f9e35e
debugger.md      3d0e9b906e5f5e29e76758cf5b170023c5cbd9f2d908bfd8263043e60d342f87
LICENSE         f89abb55d9f073f38f1703e4518f0613c788c6174be7f13b8dfe48a1c076c746
```

上游采用 MIT License，Copyright (c) 2024 Seth Hobson。完整许可保留于 [LICENSE.wshobson-agents](LICENSE.wshobson-agents)。这些 agent 配置的上游归属与许可说明不改变项目其他文件的许可。
