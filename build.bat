@echo off
echo 正在执行 make clean...
make clean
if errorlevel 1 (
    echo 错误: make clean 失败
    exit /b 1
)

echo.
echo 正在执行 make...
make
if errorlevel 1 (
    echo 错误: make 失败
    exit /b 1
)

echo.
echo 正在执行 make installdev...
make installdev
if errorlevel 1 (
    echo 错误: make installdev 失败
    exit /b 1
)

echo.
echo 所有步骤完成！
pause

