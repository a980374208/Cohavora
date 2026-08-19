@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%"

where python >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Python is not found in system PATH.
    echo Please install Python 3.8+ and ensure python is available.
    popd
    exit /b 1
)

python prepare.py %*
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Dependencies preparation and build failed.
    popd
    exit /b 1
)

popd
exit /b 0
