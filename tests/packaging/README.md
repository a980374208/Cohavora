# Windows 安装包验证

`verify_installer.py` 从当前 `packaging/windows/CohavoraInstaller.iss` 提取实际占用检测代码，
验证空闲、缺失文件、同安装目录占用、其他安装目录进程，以及三类编译预检失败。
生成的探针始终在 `InitializeSetup` 返回 False，不执行产品安装或卸载。

```powershell
python tests/packaging/verify_installer.py --iscc 'D:/software/Inno Setup 7/ISCC.exe' --build-dir out/build/windows-vs2026-release/src/app/Release --output out/installer-check-new
```

输出目录必须不存在。构建目录需有 Cohavora.exe 和两个 renderer DLL。
Restart Manager 需要可用的本机用户环境；`RmStartSession` 失败时保留日志，不能算产品验证通过。

来源及原始脚本哈希见 `../runtime/tool_migration.json`。
