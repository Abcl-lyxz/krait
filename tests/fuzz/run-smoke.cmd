@echo off
rem 60-second fuzz smoke (ADR-0010). Prefers the clang-cl primary preset;
rem falls back to MSVC's experimental /fsanitize=fuzzer when clang-cl is
rem absent. Exit code 0 = zero crashes.
setlocal

rem Enter a VS x64 dev environment unless we already are in one (CI enters it
rem in a workflow step). vswhere is the only stable locator — the VS install
rem path varies by edition, year and drive, and CI runners are not Community.
rem Test the ARCH, not merely "am I in a dev shell": the default "Developer
rem Command Prompt for VS" shortcut is x86, and it sets VCINSTALLDIR too. A
rem 32-bit fuzz build has different pointer width and overflow behaviour from
rem what ships, so it would prove nothing about the shipped parser.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_vc
if defined VSCMD_ARG_TGT_ARCH echo ERROR: this shell targets %VSCMD_ARG_TGT_ARCH%; run the smoke from an x64 developer prompt. & exit /b 1
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" echo ERROR: vswhere.exe not found; is Visual Studio installed? & exit /b 1
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR echo ERROR: no Visual Studio install with the x64 C++ toolset. & exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 echo ERROR: vcvars64.bat failed in "%VSDIR%". & exit /b 1
:have_vc
cd /d "%~dp0..\.."

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
