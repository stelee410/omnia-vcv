@echo off
REM 强制更新 SVG 文件到 VCV Rack 插件目录
REM 这个脚本会删除旧的 SVG 文件并复制新的

set RACK_USER_DIR=%LOCALAPPDATA%\Rack2
set PLUGINS_DIR=%RACK_USER_DIR%\plugins-win-x64
set PLUGIN_RES_DIR=%PLUGINS_DIR%\PureFreq\res

echo ========================================
echo 强制更新 ChordSynth.svg
echo ========================================
echo.

echo 插件目录: %PLUGIN_RES_DIR%
echo.

if not exist "%PLUGIN_RES_DIR%" (
    echo 错误: 插件目录不存在！
    echo 请先运行 build.bat 或 make installdev 安装插件
    pause
    exit /b 1
)

REM 检查源文件
if not exist "res\ChordSynth.svg" (
    echo 错误: 源文件 res\ChordSynth.svg 不存在！
    pause
    exit /b 1
)

REM 删除旧的 SVG 文件（如果存在）
if exist "%PLUGIN_RES_DIR%\ChordSynth.svg" (
    echo 删除旧的 SVG 文件...
    del /F /Q "%PLUGIN_RES_DIR%\ChordSynth.svg"
)

REM 复制新的 SVG 文件
echo 复制新的 SVG 文件...
copy /Y "res\ChordSynth.svg" "%PLUGIN_RES_DIR%\ChordSynth.svg"

if errorlevel 1 (
    echo 错误: 复制失败！
    pause
    exit /b 1
)

echo.
echo ========================================
echo SVG 文件已更新！
echo ========================================
echo.
echo 重要提示：
echo 1. 请完全关闭 VCV Rack（如果正在运行）
echo 2. 重新启动 VCV Rack
echo 3. 如果背景仍然是黑色，请尝试：
echo    - 在 VCV Rack 中：Settings - Plugins - 找到 PureFreq
echo    - 右键点击模块 - 选择 "Refresh" 或 "Reload"
echo    - 或者完全退出 VCV Rack，删除缓存后重启
echo.
echo 插件目录中的 SVG 文件位置：
echo %PLUGIN_RES_DIR%\ChordSynth.svg
echo.
pause

