@echo off
rem 60-second fuzz smoke (ADR-0010). Prefers the clang-cl primary preset;
rem falls back to MSVC's experimental /fsanitize=fuzzer when clang-cl is
rem absent. Exit code 0 = zero crashes.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d %~dp0..\..

where clang-cl >nul 2>nul
if %errorlevel%==0 (set PRESET=fuzz) else (set PRESET=fuzz-msvc)
echo using preset %PRESET%

cmake --preset %PRESET%
if errorlevel 1 exit /b 1
cmake --build --preset %PRESET%
if errorlevel 1 exit /b 1

node tools\extract-seeds.mjs build\%PRESET%\corpus
if errorlevel 1 exit /b 1

rem Replay committed regressions first (empty until the first crash is found).
if exist tests\fuzz\regressions\*.bin (
    build\%PRESET%\tests\fuzz\parser-fuzz.exe -runs=0 tests\fuzz\regressions
    if errorlevel 1 exit /b 1
)

build\%PRESET%\tests\fuzz\parser-fuzz.exe -max_total_time=60 -print_final_stats=1 build\%PRESET%\corpus
