# Cohavora Windows x64 Qt/Libraries 源码获取与重编

本文与以下正式依赖归档配套，Release 发布时应作为同级资产上传：

- 归档：`cohavora-libraries-win64-qt-5.15.18-20260925.zip`
- 大小：`1,217,880,494` 字节
- SHA-256：`491853cce06a78cee7427c243a9e21930ea8ed08af399fe60c6af258db650215`
- 归档根目录：`Libraries/win64`

本文用于定位上游源码、重建归档中的库，以及在修改源码后生成新的正式归档。归档是可直接使用的构建输入，不是所有上游仓库的完整历史副本。

## 1. 归档内容和源码边界

`Libraries/win64` 当前包含：

| 目录 | 版本或提交 | 归档内容 | 上游源码 |
|---|---|---|---|
| `Qt-5.15.18` | Qt 5.15.18 | 静态 Qt 安装树、插件、CMake/qmake 元数据、Linguist 工具和中文 catalog | `https://github.com/qt/qt5.git` |
| `zlib` | 1.3.1 / `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf` | 源码及 Debug/Release 构建产物 | `https://github.com/madler/zlib.git` |
| `mozjpeg` | 4.1.5 / `6c9f0897afa1c2738d7222a0a9ab49e8b536a267` | 源码及 Debug/Release 构建产物 | `https://github.com/mozilla/mozjpeg.git` |
| `tg_angle` | `e3f59e8d0c3e68385572e276420715a00d8754b4` | Telegram Desktop 的 ANGLE 源码快照及 Debug/Release 构建产物 | `https://github.com/desktop-app/tg_angle.git` |

完整重建 Qt 还需要以下输入。它们参与 Qt 的静态链接，但没有作为独立顶层目录放进本归档：

| 输入 | 版本或提交 | 上游源码 |
|---|---|---|
| Qt 补丁集 | `4519c85c924b9da81f29d4aac045886f896ee479` | `https://github.com/desktop-app/patches.git` |
| OpenSSL | 3.2.1 / `a7e992847de83aa36be0c399c89db3fb827b0be2` | `https://github.com/openssl/openssl.git` |
| libwebp | 1.6.0 / `4fa21912338357f89e4fd51cf2368325b59e9bd9` | `https://github.com/webmproject/libwebp.git` |

Qt 5.15.18 的固定提交如下：

| 仓库 | 提交 |
|---|---|
| `qt5` superproject | `2c7b48ab7d02c13e36266954d07bc8df5789c086` |
| `qtbase` | `49adb85d34918034e0d6a4c23817407103fb9f73` |
| `qtimageformats` | `21492b6e2ef4a5d913bdce1d34ec32e6e7ed53ae` |
| `qtsvg` | `119bb997151cfcab6b13246b8edd2783ff1df8d5` |
| `qttools` | `83b91bf9dbdde0bd51c1906168ad4b4ecef54420` |
| `qttranslations` | `3cbcceb8e3e2e63a4022f1be946c7118c527a83e` |

`qtbase` 在上述提交上应用了补丁集的 `qtbase_5.15.18/*.patch`。定位问题时必须同时考虑上游提交和这些补丁，不能只检出原始 Qt tag。

## 2. 工具链

在 **x64 Native Tools Command Prompt for Visual Studio** 中执行后续命令。当前正式归档使用的工具链指纹为：

- Windows x64
- MSVC `19.50.35730`，工具集目录 `14.50.35717`
- Windows SDK `10.0.26100.0`
- CMake `4.3.0`
- `Ninja Multi-Config`
- Qt `jom 1.1.3`
- Perl，用于构建 OpenSSL
- NASM，用于构建 mozjpeg SIMD 代码
- Git 和 Python 3

修改源码后可使用兼容的更新版工具链，但必须保持 x64、静态运行库 `/MT`、Qt static/static-runtime 和当前依赖版本。若要求二进制逐字节一致，还必须使用相同编译器、SDK、构建工具和绝对源码布局。

本文命令使用以下目录布局：

```text
C:\work\cohavora-deps\
  Libraries\win64\
    qt_5.15.18\       # Qt 源码
    Qt-5.15.18\       # Qt 安装前缀，最终进入正式归档
    patches\
    zlib\
    mozjpeg\
    tg_angle\
    openssl3\
    libwebp\
  ThirdParty\jom\
```

在命令提示符中初始化路径：

```bat
set "ROOT=C:\work\cohavora-deps"
set "LIBS_DIR=%ROOT%\Libraries\win64"
set "QT_SRC=%LIBS_DIR%\qt_5.15.18"
set "QT_PREFIX=%LIBS_DIR%\Qt-5.15.18"
set "PATH=%ROOT%\ThirdParty\jom;%PATH%"
mkdir "%LIBS_DIR%"
cd /d "%LIBS_DIR%"
```

## 3. 获取固定源码

```bat
git clone https://github.com/madler/zlib.git zlib
git -C zlib checkout 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf

git clone https://github.com/mozilla/mozjpeg.git mozjpeg
git -C mozjpeg checkout 6c9f0897afa1c2738d7222a0a9ab49e8b536a267

git clone https://github.com/desktop-app/tg_angle.git tg_angle
git -C tg_angle checkout e3f59e8d0c3e68385572e276420715a00d8754b4

git clone https://github.com/desktop-app/patches.git patches
git -C patches checkout 4519c85c924b9da81f29d4aac045886f896ee479

git clone https://github.com/openssl/openssl.git openssl3
git -C openssl3 checkout a7e992847de83aa36be0c399c89db3fb827b0be2

git clone https://github.com/webmproject/libwebp.git libwebp
git -C libwebp checkout 4fa21912338357f89e4fd51cf2368325b59e9bd9

git clone https://github.com/qt/qt5.git qt_5.15.18
git -C qt_5.15.18 checkout 2c7b48ab7d02c13e36266954d07bc8df5789c086
git -C qt_5.15.18 submodule update --init --recursive qtbase qtimageformats qtsvg qttools qttranslations
```

检出后用上一节的提交表核对每个 Qt 子模块。`qt5` 的 `v5.15.18-lts-lgpl` tag 指向本说明固定的 superproject 提交。

## 4. 构建 zlib、mozjpeg 和 tg_angle

### zlib 1.3.1

```bat
cmake -S "%LIBS_DIR%\zlib" -B "%LIBS_DIR%\zlib" -G "Ninja Multi-Config" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>" ^
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW ^
  -DCMAKE_C_FLAGS="/DZLIB_WINAPI" ^
  -DZLIB_BUILD_EXAMPLES=OFF
cmake --build "%LIBS_DIR%\zlib" --config Debug
cmake --build "%LIBS_DIR%\zlib" --config Release
```

关键产物：

```text
zlib\Debug\zlibstaticd.lib
zlib\Release\zlibstatic.lib
```

### mozjpeg 4.1.5

```bat
cmake -S "%LIBS_DIR%\mozjpeg" -B "%LIBS_DIR%\mozjpeg" -G "Ninja Multi-Config" ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  -DWITH_JPEG8=ON ^
  -DPNG_SUPPORTED=OFF
cmake --build "%LIBS_DIR%\mozjpeg" --config Debug
cmake --build "%LIBS_DIR%\mozjpeg" --config Release
```

关键产物：

```text
mozjpeg\Debug\jpeg-static.lib
mozjpeg\Release\jpeg-static.lib
```

### tg_angle

```bat
cmake -S "%LIBS_DIR%\tg_angle" -B "%LIBS_DIR%\tg_angle\out" -G "Ninja Multi-Config" ^
  -DTG_ANGLE_SPECIAL_TARGET=win64 ^
  -DTG_ANGLE_ZLIB_INCLUDE_PATH="%LIBS_DIR%\zlib"
cmake --build "%LIBS_DIR%\tg_angle\out" --config Debug
cmake --build "%LIBS_DIR%\tg_angle\out" --config Release
```

关键产物：

```text
tg_angle\out\Debug\tg_angle.lib
tg_angle\out\Release\tg_angle.lib
```

需要修改 ANGLE 时，修改 `tg_angle/src` 下的源码并重建该目标。Qt 链接的是 `tg_angle.lib`，因此改变其公开头文件或 ABI 后也要重建 Qt。

## 5. 构建 Qt 的外部静态依赖

### OpenSSL 3.2.1

```bat
cd /d "%LIBS_DIR%\openssl3"
perl Configure no-shared no-tests debug-VC-WIN64A /FS
jom -j%NUMBER_OF_PROCESSORS% build_libs
mkdir out.dbg
move libcrypto.lib out.dbg
move libssl.lib out.dbg
move ossl_static.pdb out.dbg
jom clean
move out.dbg\ossl_static.pdb out.dbg\ossl_static

perl Configure no-shared no-tests VC-WIN64A /FS
jom -j%NUMBER_OF_PROCESSORS% build_libs
mkdir out
move libcrypto.lib out
move libssl.lib out
move ossl_static.pdb out
```

### libwebp 1.6.0

```bat
cd /d "%LIBS_DIR%\libwebp"
nmake /f Makefile.vc CFG=debug-static OBJDIR=out RTLIBCFG=static all
nmake /f Makefile.vc CFG=release-static OBJDIR=out RTLIBCFG=static all
copy /y out\release-static\x64\lib\libwebp.lib out\release-static\x64\lib\webp.lib
copy /y out\release-static\x64\lib\libwebpdemux.lib out\release-static\x64\lib\webpdemux.lib
copy /y out\release-static\x64\lib\libwebpmux.lib out\release-static\x64\lib\webpmux.lib
```

这些目录虽然不在正式 ZIP 的顶层清单中，但 Qt 的配置和静态插件重编需要它们。不要用不同 OpenSSL 或 WebP 版本替换后仍沿用原归档哈希。

## 6. 应用 Qt 补丁并构建 Qt

先在未修改的 `qtbase` 提交上按编号应用固定补丁：

```bat
for /r "%LIBS_DIR%\patches\qtbase_5.15.18" %P in (*.patch) do git -C "%QT_SRC%\qtbase" apply "%P"
```

然后从 `qt5` superproject 根目录配置和构建：

```bat
cd /d "%QT_SRC%"
set "ANGLE_DIR=%LIBS_DIR%\tg_angle"
set "ANGLE_LIBS_DIR=%ANGLE_DIR%\out"
set "MOZJPEG_DIR=%LIBS_DIR%\mozjpeg"
set "OPENSSL_DIR=%LIBS_DIR%\openssl3"
set "OPENSSL_LIBS_DIR=%OPENSSL_DIR%\out"
set "ZLIB_LIBS_DIR=%LIBS_DIR%\zlib"
set "WEBP_DIR=%LIBS_DIR%\libwebp"

call configure.bat -prefix "%QT_PREFIX%" ^
  -debug-and-release ^
  -force-debug-info ^
  -opensource ^
  -confirm-license ^
  -static ^
  -static-runtime ^
  -opengl es2 -no-angle ^
  -I "%ANGLE_DIR%\include" ^
  -D "KHRONOS_STATIC=" ^
  -D "DESKTOP_APP_QT_STATIC_ANGLE=" ^
  QMAKE_LIBS_OPENGL_ES2_DEBUG="%ANGLE_LIBS_DIR%\Debug\tg_angle.lib %ZLIB_LIBS_DIR%\Debug\zlibstaticd.lib d3d9.lib dxgi.lib dxguid.lib" ^
  QMAKE_LIBS_OPENGL_ES2_RELEASE="%ANGLE_LIBS_DIR%\Release\tg_angle.lib %ZLIB_LIBS_DIR%\Release\zlibstatic.lib d3d9.lib dxgi.lib dxguid.lib" ^
  -egl ^
  QMAKE_LIBS_EGL_DEBUG="%ANGLE_LIBS_DIR%\Debug\tg_angle.lib %ZLIB_LIBS_DIR%\Debug\zlibstaticd.lib d3d9.lib dxgi.lib dxguid.lib Gdi32.lib User32.lib" ^
  QMAKE_LIBS_EGL_RELEASE="%ANGLE_LIBS_DIR%\Release\tg_angle.lib %ZLIB_LIBS_DIR%\Release\zlibstatic.lib d3d9.lib dxgi.lib dxguid.lib Gdi32.lib User32.lib" ^
  -openssl-linked ^
  -I "%OPENSSL_DIR%\include" ^
  OPENSSL_LIBS_DEBUG="%OPENSSL_LIBS_DIR%.dbg\libssl.lib %OPENSSL_LIBS_DIR%.dbg\libcrypto.lib Ws2_32.lib Gdi32.lib Advapi32.lib Crypt32.lib User32.lib" ^
  OPENSSL_LIBS_RELEASE="%OPENSSL_LIBS_DIR%\libssl.lib %OPENSSL_LIBS_DIR%\libcrypto.lib Ws2_32.lib Gdi32.lib Advapi32.lib Crypt32.lib User32.lib" ^
  -I "%MOZJPEG_DIR%" ^
  LIBJPEG_LIBS_DEBUG="%MOZJPEG_DIR%\Debug\jpeg-static.lib" ^
  LIBJPEG_LIBS_RELEASE="%MOZJPEG_DIR%\Release\jpeg-static.lib" ^
  -system-webp ^
  -I "%WEBP_DIR%\src" ^
  -L "%WEBP_DIR%\out\release-static\x64\lib" ^
  -mp ^
  -no-feature-netlistmgr ^
  -nomake examples ^
  -nomake tests ^
  -platform win32-msvc

jom -j%NUMBER_OF_PROCESSORS%
jom -j%NUMBER_OF_PROCESSORS% install
```

必须检查 `config.summary` 至少满足：

- `debug_and_release`、`static`、`static_runtime`
- C++17
- OpenSSL linked
- system libjpeg 和 system libwebp
- OpenGL ES 2.0/EGL 使用 `tg_angle`
- 构建 `libs tools`

Qt 的 `.prl`、qmake 和部分 CMake 元数据会记录绝对路径。更换工作根目录后应重新运行 configure/install；不要通过批量字符串替换伪造一个新的 Qt 安装树。

## 7. 构建 Linguist 工具和中文 catalog

正式归档额外要求与 Qt 5.15.18 完全匹配的 `lupdate.exe`、`lrelease.exe` 和 `qtbase_zh_CN.qm`。

在 Qt 安装完成后生成 qttools 的 Makefile：

```bat
set "QTTOOLS_SRC=%QT_SRC%\qttools"
set "QTTOOLS_BUILD=%ROOT%\build\qttools-5.15.18"
mkdir "%QTTOOLS_BUILD%"
cd /d "%QTTOOLS_BUILD%"
"%QT_PREFIX%\bin\qmake.exe" -o Makefile "%QTTOOLS_SRC%\qttools.pro" "CONFIG+=release"
nmake qmake_all
nmake /f src\linguist\lrelease\Makefile.Release
nmake /f src\linguist\lupdate\Makefile.Release
copy /y bin\lrelease.exe "%QT_PREFIX%\bin\lrelease.exe"
copy /y bin\lupdate.exe "%QT_PREFIX%\bin\lupdate.exe"
```

从固定 `qttranslations` 源码生成简体中文 catalog：

```bat
"%QT_PREFIX%\bin\lrelease.exe" ^
  "%QT_SRC%\qttranslations\translations\qtbase_zh_CN.ts" ^
  -qm "%QT_PREFIX%\translations\qtbase_zh_CN.qm"
```

当前 catalog 的 SHA-256 为：

```text
98fd4b97dbec8af5cdd15f85a16c33efc77301b1aeb0469daafc04b9ab2f3570
```

修改 `qtbase_zh_CN.ts` 后只需重新运行 `lrelease`；修改 Linguist 源码后需要重新构建相应工具。

## 8. 修改、验证和重新归档

1. 在固定源码提交上完成修改，记录 patch 或新提交；不要只修改生成的 `.lib`、`.exe` 或安装头文件。
2. 重建直接受影响的库。若公开头文件、ABI 或 Qt 静态依赖变化，重新配置并构建 Qt。
3. 将最终目录放到 Cohavora 的 `deps/Libraries/win64`。
4. 先验证项目要求的文件，再生成确定性归档：

```bat
cd /d C:\path\to\Cohavora
python build\prepare\package_libraries.py
```

5. 记录脚本输出的新文件数、字节数和 SHA-256；`build/prepare/libraries-archive.json` 会同步更新。
6. 使用新 tag 和新文件名发布，不要覆盖已经固定哈希的旧资产。
7. 在干净环境运行：

```bat
python build\prepare\prepare.py
cmake --preset windows-vs2026-release
cmake --build --preset release --parallel 2
```

`package_libraries.py` 会固定条目顺序、时间戳、权限和压缩级别，并在完成后读取所有 ZIP 条目验证 CRC。`configure.py --prepare-deps` 还会验证正式元数据中的文件大小、SHA-256、文件数、目录前缀和必需文件指纹。

## 9. 快速核验正式资产

PowerShell：

```powershell
$archive = 'cohavora-libraries-win64-qt-5.15.18-20260925.zip'
(Get-Item -LiteralPath $archive).Length
(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
```

预期输出：

```text
1217880494
491853cce06a78cee7427c243a9e21930ea8ed08af399fe60c6af258db650215
```

如版本、编译参数、补丁、编译器或任一文件发生变化，应生成新的归档和校验元数据，而不是继续使用本文记录的旧哈希。
