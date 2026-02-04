@echo off
REM 重新编译并安装插件，确保SVG文件被正确复制

echo ========================================
echo 重新编译并安装 PureFreq 插件
echo ========================================
echo.

REM 清理旧的构建文件
echo [1/4] 清理旧的构建文件...
make clean
if errorlevel 1 (
    echo 警告: make clean 失败，继续...
)

echo.
echo [2/4] 编译插件...
make
if errorlevel 1 (
    echo 错误: 编译失败！
    pause
    exit /b 1
)

echo.
echo [3/4] 检查编译结果...
if not exist "plugin.dll" (
    echo 错误: plugin.dll 未生成！
    pause
    exit /b 1
)
echo ✓ plugin.dll 已生成

if not exist "res\ChordSynth.svg" (
    echo 错误: ChordSynth.svg 不存在！
    pause
    exit /b 1
)
echo ✓ ChordSynth.svg 存在

echo.
echo [4/4] 安装到 VCV Rack 插件目录...
make installdev
if errorlevel 1 (
    echo 错误: 安装失败！
    pause
    exit /b 1
)

echo.
echo ========================================
echo 安装完成！
echo ========================================
echo.
echo 重要提示：
echo 1. 请完全关闭 VCV Rack（如果正在运行）
echo 2. 重新启动 VCV Rack
echo 3. 如果背景仍然是黑色，请尝试：
echo    - 在 VCV Rack 中：Settings - Plugins - 找到 PureFreq - 点击 Refresh
echo    - 或者清除 VCV Rack 缓存后重启
echo.
pause

