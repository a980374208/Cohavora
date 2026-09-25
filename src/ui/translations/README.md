# 界面翻译

界面源语言为英文，静态文案使用 `QCoreApplication::translate("MeetingUI", "English source text")`。完整句子使用 `%1` 等占位符，中文译文维护在 `cohavora_zh_CN.ts` 中。

英文检查已完成，启动时默认使用 `zh_CN` 并加载中文翻译。中文 TS 编译为 QM 后嵌入客户端，无需手动复制应用译文文件。

使用 `--language=en_US` 或 `--language en_US` 可查看英文源文案；`--language=zh_CN` 可显式选择简体中文，重启生效。

更新翻译条目：

```powershell
cmake --build build-debug --config Debug --target update_ui_translations
python tests/verify_ui_translations.py
```

CMake 只在当前 Qt SDK 的 `bin` 目录自动查找 `lupdate` 和 `lrelease`，并校验工具版本与 SDK 一致；SDK 未附带这些工具时，可设置 `COHAVORA_LUPDATE_EXECUTABLE`、`COHAVORA_LRELEASE_EXECUTABLE`。构建时将 TS 编译成 QM 并嵌入客户端。开发配置缺少 Linguist 工具时保留英文源文案显示。

构建时同时从当前 Qt SDK 的 `translations` 目录查找并嵌入 Qt 简体中文资源，供标准按钮、文件对话框使用；可通过 `COHAVORA_QT_ZH_CN_TRANSLATION` 指定资源路径。正式发布配置设置 `COHAVORA_REQUIRE_UI_TRANSLATIONS=ON`，缺少匹配工具、应用目录或 Qt 中文资源时配置失败。

可执行文件旁 `translations` 目录中的 `cohavora_<locale>.qm` 优先于内置目录；新文件不存在时，可兼容加载外部 `livekit_meeting_<locale>.qm`。内置资源只包含 Cohavora 文件名。Qt 标准控件的 `qt_<locale>.qm` / `qtbase_<locale>.qm` 及其依赖也可放在此目录。缺失条目回退到英文源文案。

用户输入、服务端内容、设备名称、协议值和资源标识保留原始含义。转发到控制台的原生诊断日志使用固定英文，避免依赖界面语言。

界面 QSS 位于 `../styles/application.qss` 和 `../styles/widgets.qss`，通过 Qt 资源在 `AppTheme::install()` 中一次加载。控件调用 `AppTheme::setStyleVariant()` 选择状态，不设置局部样式表。`uiStyle` 的直接匹配使用更高优先级，以保留局部状态样式覆盖容器样式的行为。
