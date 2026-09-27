# Windows UIA 控件回归

使用真实 `MeetingLogConsoleWindow` 和 `WhiteboardPanel`，复用现有 Qt 测试链接环境。
fixture 只创建窗口、注入两条已注册的安全日志并运行事件循环，不创建 Room、
不启动媒体或日志落盘 pipeline。所有受测操作均由独立 PowerShell 进程通过
Windows UIA 执行，不调用 Qt 测试后门、鼠标坐标、截图、OCR 或 SendInput。

## 运行

要求 Windows x64、Windows PowerShell 5.1（系统自带 .NET UIAutomationClient）、
已解锁的交互式 Default 桌面。建议在专用桌面测试 worker 上执行；运行期间不要
锁屏、切换用户或操作 fixture。脚本强制使用 Qt `windows` backend，结束时恢复
调用进程的 Qt 环境变量。无需账号、服务器或设备。

在仓库根目录使用现有 build tree：

```powershell
cmake --build out/build/windows-vs2026-dev --config Debug --target uia_console_fixture -j 8
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_console.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_console_fixture.exe -Language zh_CN
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_console.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_console_fixture.exe -Language en_US
```

加 `-ProbeOnly` 仅探测树（结果为 `PROBED`，不是测试 PASS）。
`-OutputDirectory` 指定产物根目录；每次运行创建带时间、语言和 PID 的独立目录，
避免旧 PASS 覆盖或冒充本次结果。

默认不注册 CTest，fixture 也是 `EXCLUDE_FROM_ALL`，不进入常规构建。
专用交互桌面 build tree 可显式 configure `-DLIVEKIT_REGISTER_UIA_TESTS=ON`，
构建 fixture 后运行：

```powershell
ctest --test-dir <interactive-build-tree> -C Debug -L INTERACTIVE_DESKTOP --output-on-failure
```

当前工作区 opt-in 注册日志控制台、白板工具栏、清空确认框、文件流程，以及入会、设置、会议三个新增中文流程（共七个），标签为 `UIA;INTERACTIVE_DESKTOP`，串行执行、
超时 120 秒。脚本退出码 77 表示 `NOT_RUN`，CTest 显示为 skipped，不能计作 PASS。
不要在通用 CI build tree 打开这个选项。英文流程使用上述直接命令。
脚本每个状态等待上限 10 秒，fixture 自身有 90 秒生命周期上限；脚本最终只终止
自己创建并持有进程句柄的 fixture。

## 定位与断言

实际 Qt 5.15.18 Windows 树观察到的 AutomationId 形式为：

```text
adaptiveDialogScroll.qt_scrollarea_viewport.adaptiveDialogContent.consoleTextFilter
```

不能假设 objectName 就是完整 AutomationId。定位在新启动进程唯一顶层窗口
子树内进行，匹配完整 ID 或 `.` 分隔的末段，要求唯一、正确角色、非空 Name、
正确 enabled/offscreen 状态。Name 仅检查非空，不用中英文文案定位。
根窗口名和父级 ID 路径不作为固定选择器。无所需 Pattern 直接
`MISSING_PATTERN` 失败，不回退其他交互方式。

| 流程 | 控件级操作 | UIA 断言 |
|---|---|---|
| 发现 | 查询树及 Pattern | 12 个控件的唯一性、角色、名称、可用及屏幕状态 |
| 过滤 | Value.SetValue | 输入值；TextPattern 中两条、单条、零条及恢复两条 |
| 自动滚屏 | Toggle.Toggle | On → Off → On（只证明开关状态，不证明滚动像素） |
| 清空 | Invoke.Invoke | 正文为空；重新过滤后也无缓存日志 |
| 无 pipeline | 只读查询 | 保存日志、诊断模式均 disabled |

正文还检查 Value.IsReadOnly；输入框检查可写。下拉框在本轮仅验证可访问性，
不声明 Selection 操作已覆盖。探测发现 ComboBox 自身暴露 Value/Invoke，内部
List 和 ListItem 分别暴露 Selection/SelectionItem；没有假定 ComboBox 有
ExpandCollapse。复制按钮只检查 Invoke 存在，不改动系统剪贴板。

产物包括 `initial-tree.json`、`final-tree.json`（成功）、`failure-tree.json`
（失败且 provider 仍可访问）以及 `result.json`。树保存父子 runtime ID、角色、
名称、状态、支持的 Pattern，以及 Value/Toggle/Text 读数；结果保存失败阶段、
检查列表和 fixture/脚本/相关源码 SHA-256。探测内容仅来自 fixture 进程。
fixture 意外退出时可能无法保存树，但仍记录退出码和失败阶段。

## 白板工具栏

`test_console.ps1` 与 `test_whiteboard.ps1` 共用 `run_desktop.ps1` 的桌面检测、
进程隔离、查找、等待和证据输出。入口显式转发退出码，FAIL/77 不会变成成功。

```powershell
cmake --build out/build/windows-vs2026-dev --config Debug --target uia_whiteboard_fixture -j 8
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_whiteboard.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_whiteboard_fixture.exe -Language zh_CN
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_whiteboard.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_whiteboard_fixture.exe -Language en_US
```

独立 fixture 创建空白的本地白板并预设 150% 缩放；没有协作 transport，
不发送画布事件。当前通过范围：

- 10 个工具的 Toggle 切换及互斥状态；重复 Toggle 当前工具仍保持唯一选中。
- 线宽和字号的 RangeValue 初值、上下界、修改及恢复。
- Fit 按钮 Invoke 后，缩放 Value 从非 fit 初值变成列表中的 fit 选项。
- Add page Invoke 后出现两个页面选项，ComboBox Value 指向新页面。
- 颜色、缩放、页面通过 SelectionItem.Select 提交；颜色和页面还分别验证
  ListItem.Invoke，要求唯一选中状态与 ComboBox 当前 Value 同步。
- Undo/Redo 在空文档中 disabled；其余受检按钮的角色、名称、可用状态和 Pattern。

真实树中，工具按钮角色是 CheckBox，不是 Button；修改前十个按钮的
AutomationId 均为 `whiteboardTool`。现已设为 `whiteboardToolPen` 等唯一标识。
颜色、线宽、字号、缩放原有可访问名称，但 AutomationId 为空，现已补齐。
工具状态不等于实际画出的对象类型，RangeValue 不等于实际笔画宽度，缩放 Value
也不证明渲染比例或像素；这些仍由直接 Qt/渲染测试提供独立证据。

**选择提交已在项目侧适配：** Qt 5.15.18 的 Windows SelectionItem provider
把 Select 映射为列表项 `toggleAction()`，原默认列表只更新高亮/selection model，
未提交 ComboBox。白板的三个下拉框现在使用项目侧 QListView 可访问性接口，
将未选中项的 toggle 和 press 动作提交至 QComboBox，并保持单选语义。
接口只用 Qt 公共 API；普通鼠标/键盘行为继续由 QListView/QComboBox 处理，
不会把悬停高亮当作提交。

提交排入 Qt 事件循环，持有 QPersistentModelIndex，并在执行时检查模型、
控件存活及 enabled/selectable。切页可同步清空并重建选项，测试因此重新查询
ListItem，而不保留已移除的 provider。关闭弹层时逻辑选项仍可枚举，状态为
offscreen；当前选中状态取自 ComboBox 已提交的值。

旧失败产物保留，选择提交现在是默认白板流程的必测项。
原 `-ProbeComboSelection` 参数仍兼容，运行同一套必测流程，预期 PASS：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_whiteboard.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_whiteboard_fixture.exe -ProbeComboSelection
```

选项探针使用进程限定的 ComboBox 内部列表及声明的选项顺序，不按翻译文案定位。
测试不以坐标、键盘模拟或 Qt 内部调用绕过 UIA。未修改 Qt/vendor。
直接 Qt 回归另行验证悬停不提交、提交信号、禁用后拒绝、模型替换、销毁取消、
页面模型重建，以及选择缩放后 canvas 的实际 zoom 状态。

## 清空确认框

```powershell
cmake --build out/build/windows-vs2026-dev --config Debug --target uia_whiteboard_clear_fixture -j 8
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_whiteboard_clear.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_whiteboard_clear_fixture.exe -Language zh_CN
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_whiteboard_clear.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_whiteboard_clear_fixture.exe -Language en_US
```

该 fixture 启动时直接向本地模型放入一个固定对象，之后仅观察模型；受测操作
全部通过 UIA。不会通过鼠标事件、画布坐标或测试入口来触发取消/清空。
`-ProbeOnly` 在 Invoke 清空按钮后记录确认框树，结果为 PROBED，不算 PASS。

真实树中 QMessageBox 是 Window，提供 Window Pattern；两个 QPushButton 提供
Invoke。原对话框和按钮无 AutomationId，现已补齐 `whiteboardClearConfirmation`、
`whiteboardClearConfirm`、`whiteboardClearCancel`。查找限定 fixture PID，兼容
模态窗作为根窗口或主窗口子树的布局，要求唯一匹配，不依赖按钮语言。

流程检查 modal、初始焦点在 Cancel、按钮可用和 Invoke Pattern，然后分别验证：

- Cancel.Invoke：弹框关闭、主窗口恢复可用、完整模型快照不变。
- Window.Close：等同取消，完整模型快照不变。
- Confirm.Invoke：弹框关闭，同一页面对象数为 0、epoch 增长、无 Undo/Redo 历史，
  UIA 中 Undo/Redo 也 disabled。

`model-state.json.00000001` 等编号文件是 fixture 持续写出的只读观测证据，不是控制通道。
脚本核对 PID 与递增 sample，并保存 `model-before.json`、`model-after-cancel.json`、
`model-after-close.json`、`model-after-confirm.json`，以及每个确认框的树。
每 200ms 使用 QSaveFile 原子发布一个不可变样本，读取最新已完成的编号文件，
避免 Windows 上读者与替换同名文件竞争。fixture 最长 90 秒，因此最多约 450 个样本。
UIA 只证明控件状态，内容保留/移除由这些独立模型快照证实，不代表协作端已同步
或磁盘已持久化。

## 证据边界

- 当前包含控制台、白板工具栏/清空/文件及新增入会/设置/会议控件流程；覆盖与执行状态按各自证据区分，不是完整应用启动或媒体验收。
- 白板绘制/文字输入及协作：DEFERRED。
- 入会/设置、会议工具栏及分页：当前工作区已新增 join/settings/meeting 脚本和 opt-in 注册；脚本存在不等于验收 PASS，具体执行结果须核对独立 result.json 及输入哈希。
- 真实服务入会、共享、重连、日志落盘及崩溃恢复：不由本 UIA fixture 验收。此 fixture 没有真实服务、
  媒体设备或持久化 pipeline；必须另用运行探针和产物验证。
- UIA 不足以证明视频像素/音频质量、实际 codec/RTP、物理多屏坐标、网络故障、
  资源长稳、慢盘/断电或真实崩溃恢复。
- 不操作保存日志、清理历史或崩溃收集开关。Qt 设置使用临时目录；崩溃开关的
  初始状态仍由生产代码只读读取当前用户策略，不对其值作断言。

## File dialogs / artifact verification

```powershell
cmake --build out/build/windows-vs2026-dev --config Debug --target uia_whiteboard_files_fixture -j 8
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_whiteboard_files.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_whiteboard_files_fixture.exe -Language zh_CN
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_whiteboard_files.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_whiteboard_files_fixture.exe -Language en_US
```

Same EXCLUDE_FROM_ALL, opt-in registration and desktop exit-77 isolation. ProbeOnly
opens the save dialog on an existing file and records the overwrite tree; reports PROBED, never PASS.
Observed QFileDialog: Window/WindowPattern; fileNameEdit: Edit/writable Value;
accept/cancel: Button/Invoke. Previously unnamed button AutomationIds now have
whiteboardFileAccept/Cancel IDs; dialogs have whiteboardImportDialog/ExportDialog.
Selectors require PID and unique ID suffix, never localized text or input events.

The fixture creates a deterministic opaque 320x180 PNG before showing the panel,
then only observes numbered model snapshots. Its INI settings stay in the unique
output directory. UIA Value.SetValue enters absolute paths containing spaces;
Invoke cancels/submits import and export. Assertions cover modal dialogs, names,
patterns, filename roundtrip, cancellation preserving the model, added background
page and decoded dimensions, export completion and unchanged document.

Evidence includes input image.png, export image.png, dialog trees, stage model
snapshots, result.json and artifact-report.json. An independent System.Drawing
decoder checks PNG signature, dimensions and all 57600 decoded pixels against the
input, with SHA256 for both files. Images are never used for UI recognition.
Cancelled output must remain absent. Only new destinations in this run are used.
JPEG, truncated encoded payloads, oversized/unreadable inputs, page-limit replacement, remote
upload, slow disk and power-loss recovery are NOT_RUN. File pixel equality does
not establish screen rendering, video/audio quality or production readiness.

### Existing destination / overwrite

The same files scenario now also starts with a solid-magenta existing PNG distinct
from the imported background. It invokes Save on that path and discovers the nested
QMessageBox through the fixture PID. Qt's internally created Yes/No buttons originally
had no AutomationId. A lifetime-scoped production event filter names only Yes/No
message boxes belonging to this export dialog when shown, and applies AppTheme.
Qt still owns the overwrite decision and file-dialog validation; no action is injected.

Both branches require Window/Invoke patterns, unique IDs, modal state and initial No
focus. No returns to the save dialog with the destination retained; cancelling the
save dialog then preserves every original byte and the complete document snapshot.
Yes closes both dialogs, replaces the file with bytes identical to the independently
decoded/pixel-verified export, restores Export enabled state, and preserves the model.
Only fixture-owned files in the unique output directory can be overwritten.

Evidence: overwrite-cancel/confirm-confirmation-tree.json, existing-before.png,
existing-after-cancel.png, existing image.png, model-after-overwrite-*.json and
overwrite-report.json (original/cancelled/confirmed/reference SHA256). ProbeOnly leaves
the confirmation open for tree capture; runner cleanup terminates only its fixture.
This does not test concurrent external file changes, filesystem failures, slow disk,
power loss or crash durability.

### Invalid image imports

After a valid background import, the same scenario submits four existing files via
Value/Invoke: non-image bytes named invalid content.png (UnsupportedFormat), and a
valid 16x16 PNG named invalid dimensions.png (InvalidDimensions), then an 80-byte PNG
with complete IHDR/pHYs chunks and a partial IDAT named truncated image.png (DecodeFailed).
It also submits a valid 2048x2048 PNG whose random pixel data makes its encoded file
larger than 8 MiB (InputTooLarge). The fixture verifies the format, dimensions,
on-disk byte count and loader error before showing the UI.
The fixture checks that Qt recognizes its format and 320x180 dimensions but cannot
decode its pixels before starting the UI. Startup-only fixture
data includes expected-errors.json, using the current Qt translator for expected
messages. Selectors remain PID plus whiteboardStatus ID, never localized strings.
The observed QLabel is ControlType.Text; its Name supplies the accessible error text.
No unsupported Text/Value pattern is assumed or substituted with input simulation.

Each case requires the expected message to be absent before submission, then appear
in UIA after the file dialog closes. Import must become enabled again. Fresh passive
samples must preserve the full document JSON, active page, page count, background
identity/readiness and Undo/Redo state. Rejected source SHA256 must remain unchanged.
The following export and full pixel comparison additionally check that the previous
background still renders to the same PNG after both failures.

Evidence: invalid-import-report.json, invalid-*-error-tree.json,
invalid-*-model-before/after.json, expected-errors.json and both rejected inputs.
Current zh_CN/en_US flows cover these four error classes only; access-denied inputs
and real filesystem faults remain
NOT_RUN. This checks visible accessible feedback, not screen-reader announcements.

## 新增入会、设置和会议控件流程（工作区状态，2026-09-27）

`test_join.ps1`、`test_settings.ps1` 使用 `uia_entry_fixture`；`test_meeting.ps1` 使用 `test_participant_window_remediation`。入口固定中文，命令参数以各脚本为准；没有这些流程的英文验收声明。

```powershell
cmake --build out/build/windows-vs2026-dev --config Debug --target uia_entry_fixture test_participant_window_remediation -j 8
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_join.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_entry_fixture.exe
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_settings.ps1 -Fixture out/build/windows-vs2026-dev/Debug/uia_entry_fixture.exe
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/uia/test_meeting.ps1 -Fixture out/build/windows-vs2026-dev/Debug/test_participant_window_remediation.exe
```

这些是控件流程，不代表真实入会、设备、媒体、服务端或分页后 RTP/解码路数验收。文档维护任务未运行上述命令；独立执行证据仍按各 run 保存，不在此复制易过期的通过计数。

<a id="uia-evidence-20260927"></a>

### 2026-09-27 最新证据核对

本次仅读取已存在的 result.json 并比较其中记录的 fingerprints，未重新运行测试。指纹一致仅覆盖记录列出的文件，不证明所有传递依赖或整个工作区已验证。

| 流程 | 归档结果 | 本次核对与未完成边界 |
|---|---|---|
| 设置，中文 | [23:05 记录](../../out/uia-results/20260927-230508-373-settings-zh_CN-43628/result.json)：FAIL，失败前 18 条检查完成 | `Timed out: Audio page exposes AEC`；记录的二进制、脚本和源码指纹均一致。需要定位设置导航/可访问性/探针交互原因，尚不能认定为音频设备或 AEC 功能故障 |
| 会议工具栏与分页，中文 | [23:04 记录](../../out/uia-results/20260927-230434-121-meeting-zh_CN-39248/result.json)：56 项 PASS | 记录的指纹均一致；只覆盖合成参与者投影和本地控件/模型，不覆盖真实音视频、RTP、共享及服务端成员关系 |
| 入会，中文 | [22:59 记录](../../out/uia-results/20260927-225907-788-join-zh_CN-25740/result.json)：44 项 PASS | `build-debug/Debug/uia_entry_fixture.exe` 和 `run_desktop.ps1` 指纹已变；不能直接作为当前输入 PASS。真实 join/token exchange 仍需独立实服证据 |
| 白板文件，中英文 | [中文](../../out/uia-oversized/20260927-222810-978-whiteboard_files-zh_CN-37836/result.json)、[英文](../../out/uia-oversized/20260927-222938-820-whiteboard_files-en_US-17888/result.json)：各 410 项 PASS | `run_desktop.ps1`、`UiaTests.cmake` 已变，保留原版本 PASS；复用需增量核对。JPEG、不可读文件、页数上限替换、远端上传、慢盘及断电仍 NOT_RUN |

较早的失败保留。日志控制台、白板工具栏和清空流程也有历史 PASS，但公共 runner/CMake 或白板源码已有变化，不由本次盘点自动刷新为当前全套 PASS。入会/设置/会议不再登记为“尚未纳入 UIA”；其中设置应登记为已执行 FAIL。
