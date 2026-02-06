@echo off
REM 仅将 res 文件夹中的 SVG 文件复制到 VCV Rack 插件目录
REM Copy only SVG files from res/ to VCV Rack plugins

set RACK_USER_DIR=%LOCALAPPDATA%\Rack2
set PLUGINS_DIR=%RACK_USER_DIR%\plugins-win-x64
set TARGET_RES=%PLUGINS_DIR%\PureFreq\res

echo 复制 SVG 文件到 VCV Rack...
echo 目标: %TARGET_RES%

if not exist "%TARGET_RES%" (
    echo 创建 res 目录...
    mkdir "%TARGET_RES%"
)

copy /Y res\*.svg "%TARGET_RES%\"

echo.
echo SVG files installed successfully!
echo Please restart VCV Rack to see the changes.
echo Or use: Settings - Plugins - Refresh
pause
