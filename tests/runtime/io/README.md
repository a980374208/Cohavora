# 诊断文件真实 I/O 与退出回归

两个 C++ 探针取自已归档实验，保留原始测试语义。独立 CMake 只构建这两个目标，
不修改产品构建、不加入普通 CTest，不自动执行磁盘测试。

```powershell
cmake -S tests/runtime/io -B out/build/io-regression -G 'Visual Studio 18 2026' -A x64 -DJSON_INCLUDE_DIR=E:/vsSource/WebRTC/live-kit-test/out/build/windows-vs2026-dev/vcpkg_installed/x64-windows-static/include
cmake --build out/build/io-regression --config Debug --target exit_probe disk_full_probe --parallel 2
python tests/runtime/io/verify_sink_exit.py --binary out/build/io-regression/Debug/exit_probe.exe --output out/sink-exit-new
```

`JSON_INCLUDE_DIR` 指向现有依赖的 include 目录；不下载依赖。
退出测试在临时目录中检查受控写入阻塞、正常退出及锁持有者退出/被终止后的恢复。
它不是物理磁盘卡死验收。

真实满盘/取消测试要求预先准备的 **G: 空 NTFS 卷，容量严格大于 64 MiB、小于 128 MiB**。
为保留原测试保护条件，卷号目前固定；脚本不会创建、格式化或挂载卷。
允许 `System Volume Information` 和无内容、非链接的 `$RECYCLE.BIN` 系统目录；
任何回收文件或其他根目录条目都会拒绝执行，脚本不清空回收站。
输出和可执行文件应在其他卷。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/runtime/io/invoke_real_io.ps1 -Executable out/build/io-regression/Debug/disk_full_probe.exe -OutputDirectory out/disk-full-new
# 在相同隔离卷上，仅运行 oplock 阻塞取消与恢复：
powershell -NoProfile -ExecutionPolicy Bypass -File tests/runtime/io/invoke_real_io.ps1 -Executable out/build/io-regression/Debug/disk_full_probe.exe -OutputDirectory out/oplock-new -OplockOnly
```

输出目录必须不存在；每次创建唯一测试根目录，只清理该目录并拒绝 reparse point。
历史 quota 原型及失败记录保持归档，不把其旧 verdict 转换为当前代码 PASS。
