@echo off
setlocal
cd /d "%~dp0"
set "QT_PLUGIN_PATH=E:\vsSource\TDeskTop\Libraries\win64\Qt-5.15.18\plugins"
set "QT_QPA_PLATFORM_PLUGIN_PATH=E:\vsSource\TDeskTop\Libraries\win64\Qt-5.15.18\plugins\platforms"
start "" "build-debug\Release\livekit_meeting_app.exe"
