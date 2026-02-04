@echo off
REM Update SVG files to VCV Rack plugin directory
REM This script copies the SVG file without rebuilding the plugin

set RACK_USER_DIR=%LOCALAPPDATA%\Rack2
set PLUGINS_DIR=%RACK_USER_DIR%\plugins-win-x64

echo Updating ChordSynth.svg...
echo Plugins directory: %PLUGINS_DIR%

if not exist "%PLUGINS_DIR%\PureFreq" (
    echo Error: Plugin directory not found!
    echo Please run build.bat first to install the plugin.
    pause
    exit /b 1
)

REM Copy SVG file
echo Copying ChordSynth.svg...
copy /Y res\ChordSynth.svg "%PLUGINS_DIR%\PureFreq\res\ChordSynth.svg"

if errorlevel 1 (
    echo Error: Failed to copy SVG file!
    pause
    exit /b 1
)

echo.
echo SVG file updated successfully!
echo.
echo Please restart VCV Rack to see the changes.
echo Or use: Settings - Plugins - Refresh
pause

