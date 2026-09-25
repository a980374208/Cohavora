@echo OFF
setlocal

where python >nul 2>&1
if errorlevel 1 goto python_missing

python -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 8) else 1)"
if errorlevel 1 goto python_version

python "%~dp0configure.py" %*
set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" echo [ERROR] Configuration failed with exit code %EXIT_CODE%.
exit /b %EXIT_CODE%

:python_missing
echo [ERROR] Python was not found in PATH. Install Python 3.8 or newer.
exit /b 9009

:python_version
echo [ERROR] Python 3.8 or newer is required.
exit /b 1
