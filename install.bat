@echo off
REM Install plugin to VCV Rack plugins directory
REM Windows installation script

REM Get Rack plugins directory
set RACK_USER_DIR=%LOCALAPPDATA%\Rack2
set PLUGINS_DIR=%RACK_USER_DIR%\plugins-win-x64

echo Installing PureFreq plugin...
echo Plugins directory: %PLUGINS_DIR%

REM Create plugins directory if it doesn't exist
if not exist "%PLUGINS_DIR%" (
    echo Creating plugins directory...
    mkdir "%PLUGINS_DIR%"
)

REM Create plugin directory
if not exist "%PLUGINS_DIR%\PureFreq" (
    echo Creating PureFreq directory...
    mkdir "%PLUGINS_DIR%\PureFreq"
)

REM Copy files
echo Copying plugin files...
copy /Y plugin.dll "%PLUGINS_DIR%\PureFreq\"
copy /Y plugin.json "%PLUGINS_DIR%\PureFreq\"

REM Copy resources directory
if exist "res" (
    echo Copying resources...
    xcopy /E /I /Y res "%PLUGINS_DIR%\PureFreq\res\"
)

echo.
echo Installation complete!
echo.
echo Please restart VCV Rack to see the plugin.
echo If the plugin still doesn't appear:
echo 1. Check that plugin.dll was compiled successfully
echo 2. Check VCV Rack Settings - Plugins to see if there are any errors
echo 3. Try clearing VCV Rack cache and restarting
pause

