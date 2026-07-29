@echo off
rem T13 flood bench: full-grid-change frames at a fixed 4K color buffer,
rem once on the dev GPU and once on the WARP software adapter.
rem Usage: flood-report.cmd [frames]   (default 600 timed frames)
setlocal
set FRAMES=%1
if "%FRAMES%"=="" set FRAMES=600
if "%QT_ROOT%"=="" set QT_ROOT=C:\Qt\6.10.3\msvc2022_64
set PATH=%QT_ROOT%\bin;%PATH%
set QT_FORCE_STDERR_LOGGING=1
set KRAIT_BENCH=%FRAMES%
set KRAIT_BENCH_4K=1
cd /d %~dp0..\..
del /q build\bench-gpu.json build\bench-warp.json 2>nul

echo === flood bench: dev GPU (4K target, %FRAMES% frames) ===
set KRAIT_BENCH_WARP=
set KRAIT_BENCH_OUT=build\bench-gpu.json
build\dev\src\app\krait-app.exe
if errorlevel 1 exit /b 1

echo === flood bench: WARP (4K target, %FRAMES% frames) ===
set KRAIT_BENCH_WARP=1
set KRAIT_BENCH_OUT=build\bench-warp.json
build\dev\src\app\krait-app.exe
if errorlevel 1 exit /b 1

echo === reports ===
type build\bench-gpu.json
echo.
type build\bench-warp.json
echo.
