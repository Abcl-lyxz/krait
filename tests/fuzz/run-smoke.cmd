@echo off
rem 60-second fuzz smoke (ADR-0010). Exit code 0 = zero crashes.
rem
rem Every failure check below is "%ERRORLEVEL% neq 0", never "errorlevel 1":
rem cmd compares SIGNED, so `if errorlevel 1` is FALSE for a negative exit
rem code. That is not hypothetical here — CI caught a failed link reported as
rem [code=4294967295] (-1) sail straight through `if errorlevel 1`, and the
rem script cheerfully carried on to run a binary that had not been built.
rem An access violation (-1073741819) would slip through identically.
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
if %ERRORLEVEL% neq 0 echo ERROR: vcvars64.bat failed in "%VSDIR%". & exit /b 1
:have_vc
cd /d "%~dp0..\.."

rem ADR-0010 names clang-cl the primary preset, but that path does not LINK.
rem CMake drives an MSVC-like toolchain through lld-link directly instead of
rem through the compiler driver, so tests/fuzz/CMakeLists.txt's
rem target_link_options(-fsanitize=fuzzer,address) is discarded — lld-link
rem says "ignoring unknown argument" and every __asan_*/__sanitizer_cov_*
rem symbol comes back undefined. Fixing it means naming the clang_rt import
rem libraries explicitly. Until then default to the verified MSVC preset
rem rather than auto-selecting a broken toolchain the moment LLVM appears on
rem PATH; opt back in with KRAIT_FUZZ_PRESET=fuzz while fixing it.
if not defined KRAIT_FUZZ_PRESET set "KRAIT_FUZZ_PRESET=fuzz-msvc"
set "PRESET=%KRAIT_FUZZ_PRESET%"
echo using preset %PRESET%

rem Both fuzz presets read $env{VCPKG_ROOT} for their toolchain file, because
rem from T19 krait-core links utf8proc. Before that it had no dependencies and
rem this script ran fine with VCPKG_ROOT unset, so the missing variable now
rem surfaces as a confusing "Could not find a package configuration file
rem provided by utf8proc" from deep inside src/core. Say the real reason here.
rem NOTE: a toolchainFile change does NOT apply to an already-configured build
rem directory — delete build\%PRESET% if you hit that error with VCPKG_ROOT set.
if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT is not set. The %PRESET% preset needs it to resolve
    echo        utf8proc via the vcpkg toolchain. Set it to your vcpkg checkout.
    exit /b 1
)

cmake --preset %PRESET%
if %ERRORLEVEL% neq 0 exit /b 1
cmake --build --preset %PRESET%
if %ERRORLEVEL% neq 0 exit /b 1

node tools\extract-seeds.mjs build\%PRESET%\corpus
if %ERRORLEVEL% neq 0 exit /b 1

rem Replay committed regressions first (empty until the first crash is found).
rem Written flat rather than in an if(...) block on purpose: %ERRORLEVEL%
rem inside a parenthesised block expands when the block is PARSED, i.e.
rem before the command in it has run.
if not exist tests\fuzz\regressions\*.bin goto :no_regressions
build\%PRESET%\tests\fuzz\parser-fuzz.exe -runs=0 tests\fuzz\regressions
if %ERRORLEVEL% neq 0 exit /b 1
:no_regressions

build\%PRESET%\tests\fuzz\parser-fuzz.exe -max_total_time=60 -print_final_stats=1 build\%PRESET%\corpus
if %ERRORLEVEL% neq 0 exit /b 1

rem T54: the telnet negotiator is the other thing in this tree that parses
rem bytes chosen by the far end, so it gets the same treatment. Its corpus is
rem committed rather than extracted — there is no corpus of telnet cases to
rem derive one from, and fourteen hand-written seeds put the fuzzer inside the
rem interesting states instead of making it discover IAC by chance.
rem
rem 30 seconds rather than 60: the negotiator is a fraction of the parser's
rem state space and saturates quickly. Raise it if a run is ever still finding
rem new coverage at the cut-off.
rem COPIED into the build tree first, and run against the copy. libFuzzer
rem APPENDS every coverage-increasing unit to the first corpus directory it is
rem given, so pointing it at the committed seeds turns them into a corpus that
rem grows by a couple of hundred files on every run — which is exactly what
rem happened before this line existed. parser-fuzz avoids it the same way, by
rem running against build\%PRESET%\corpus.
if not exist build\%PRESET%\corpus-telnet mkdir build\%PRESET%\corpus-telnet
copy /y tests\fuzz\seeds-telnet\* build\%PRESET%\corpus-telnet\ >nul
if %ERRORLEVEL% neq 0 exit /b 1

build\%PRESET%\tests\fuzz\telnet-fuzz.exe -max_total_time=30 -print_final_stats=1 build\%PRESET%\corpus-telnet
if %ERRORLEVEL% neq 0 exit /b 1
exit /b 0
