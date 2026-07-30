@echo off
setlocal enabledelayedexpansion
rem T26 DPI gate. rules/render.md: "per-monitor DPI change mid-session without
rem restart or blur." The blur it forbids is not a rendering subtlety — it is
rem what happens when glyphs baked at N px get stretched into a colour buffer
rem sized N*dpr. So the check is: does the font actually get RE-RASTERISED at a
rem larger pixel size when the scale factor goes up, and does the grid reflow?
rem
rem QT_SCALE_FACTOR is Qt's own override and reaches the item through the same
rem path a real monitor change does (ItemDevicePixelRatioHasChanged).
rem
rem Usage: tools\dpi-check.cmd [path\to\krait-app.exe]

set "APP=%~1"
if "%APP%"=="" set "APP=build\dev\src\app\krait-app.exe"
if not exist "%APP%" (
    echo FAIL: %APP% not found - build the dev preset first
    exit /b 1
)

rem Same env contract as CMakePresets: QT_ROOT points at the Qt install, and
rem without its bin on PATH the app dies before printing anything.
if defined QT_ROOT set "PATH=%QT_ROOT%\bin;%PATH%"

set "OUT=%TEMP%\krait-dpi-check"
if not exist "%OUT%" mkdir "%OUT%"

set FAILED=0
call :run 1
call :run 2
if %FAILED%==1 goto :fail

rem The px size at 200%% must be twice the px size at 100%%, or the renderer is
rem scaling a bitmap rather than rasterising a bigger one.
set /a EXPECT=%PX_1% * 2
if not "%PX_2%"=="%EXPECT%" (
    echo FAIL: font px 100%%=%PX_1% 200%%=%PX_2%, expected %EXPECT%
    echo        the glyphs are being scaled, not re-rasterised
    goto :fail
)
if "%CELL_1%"=="%CELL_2%" (
    echo FAIL: cell metrics identical at both scales ^(%CELL_1%^) - no reflow
    goto :fail
)

echo.
echo PASS  100%%: %PX_1%px cell %CELL_1%
echo PASS  200%%: %PX_2%px cell %CELL_2%
echo PASS  dpi-check: glyphs re-rasterised and the grid reflowed
exit /b 0

:fail
echo.
echo dpi-check FAILED
exit /b 1

rem ---- :run ^<scale^> ----------------------------------------------------
rem Launches the app at a fixed scale factor and reads back the line
rem   render: px=^<N^> cell=^<W^>x^<H^> dpr=^<D^> workers=^<K^> family='...'
:run
set "SCALE=%~1"
set "LOG=%OUT%\dpi-%SCALE%.log"
echo Running at %SCALE%00%% ...
set QT_SCALE_FACTOR=%SCALE%
set KRAIT_SPIKE_AUTOQUIT=1
rem Qt sends qInfo to OutputDebugString, not stderr, when the handle is not a
rem console — which a redirect to a file is. Without this the log is empty.
set QT_ASSUME_STDERR_HAS_CONSOLE=1
"%APP%" > "%LOG%" 2>&1
set QT_SCALE_FACTOR=
set KRAIT_SPIKE_AUTOQUIT=
set QT_ASSUME_STDERR_HAS_CONSOLE=

set "PXRAW="
set "CELLRAW="
for /f "tokens=2,3 delims= " %%a in ('findstr /c:"render: px=" "%LOG%"') do (
    set "PXRAW=%%a"
    set "CELLRAW=%%b"
)
if "!PXRAW!"=="" (
    echo FAIL: no "render: px=..." line at %SCALE%00%% - see %LOG%
    set FAILED=1
    exit /b 0
)
rem Tokens arrive as px=20 and cell=12x23; keep the right-hand side of each.
for /f "tokens=2 delims==" %%x in ("!PXRAW!") do set "PX_%SCALE%=%%x"
for /f "tokens=2 delims==" %%x in ("!CELLRAW!") do set "CELL_%SCALE%=%%x"
exit /b 0
