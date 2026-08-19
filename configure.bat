@echo OFF
setlocal enabledelayedexpansion

set "FullScriptPath=%~dp0"

python "%FullScriptPath%configure.py" %*
if %errorlevel% neq 0 goto error

exit /b 0

:error
echo [ERROR] Configuration failed.
exit /b 1
