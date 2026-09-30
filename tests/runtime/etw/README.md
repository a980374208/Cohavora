# ETW 句柄工具

工具代码和 `handle-stacks.wprp` 一起纳入版本管理；输出放在调用者指定的新目录。
WPR 采集需在已获准的环境中运行，脚本不自行启动 UAC 提权进程。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/runtime/etw/invoke_handle_probe.ps1 -Executable out/build/windows-vs2026-runtime-tools/Release/test_desktop_capture_runtime.exe -OutputDirectory out/handles-new -Scenario persistent
```

`Scenario` 可选 `external`、`isolation`、`persistent`，共用采集、超时和停止流程。
`external` 使用产品默认 WGC 的真实 `Start/Stop`，每轮在 `join()` 后采样；
目标进程退出后继续存活 60 秒，每 10 秒记录一次，区分逐轮释放与进程退出清理。
`isolation` 依次生成 apartment/query/session 日志；解析时每份日志单独指定 tag。
可加 `-IsolationMode query` 或 `-IsolationMode session` 单独采集已有对照入口，默认仍采集三组。
每次采集使用唯一 WPR instance，停止时验证成功启动记录，拒绝覆盖已有记录。

```powershell
python tests/runtime/etw/extract_handle_trace.py TRACE.etl LIFECYCLE.log --tag capture
python tests/runtime/etw/compare_handle_traces.py --input-directory out/handles-new --capture capture --published published --output out/handles-comparison-new.json
python tests/runtime/etw/symbolize_handle_stacks.py out/handles-new/capture-handles.json --symbols out/build/windows-vs2026-runtime-tools/Release --output out/handles-symbols-new.json
```

解析器支持 `--xperf`、`--range START END`。比较器要求各 tag 对应的 `-handles.json`
及 xperf 导出的 `-processes.csv`，可通过 `--external TAG` 加入外部窗口结果。
解析结果的 `module_events` 保留映像加载／卸载事件，可对照最后一个存活样本检查 DLL 生命周期。
符号器支持 `--debuggers` 和可重复的 `--type`，默认只查看 ALPC Port。
使用当前 EXE 对应的 PDB 目录作为 `--symbols`；符号器启用严格符号匹配，
并在输出 JSON 旁写入同名 `.lines.json`，记录可解析地址的源码行。
重放历史采集且原路径二进制已重建时，使用 `--image-directory` 指定归档 EXE/DLL 目录，配合其原始 PDB。
没有行记录的系统帧不能据最近导出符号推断其内部实现。

证据边界：HandleDuplicate 未纳入重建；丢事件、进程生命周期、100 ms 退出区间限制仍保留。
重建句柄数不能代替系统总量；历史比较结果一致不等于当前系统真实采集已通过。
