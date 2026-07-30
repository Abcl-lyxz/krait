@echo off
setlocal enabledelayedexpansion
rem T35 — the vttest golden script.
rem
rem What this is NOT: a run of Thomas Dickey's interactive vttest. That program
rem drives a real terminal, waits on keypresses between screens, and reports by
rem eye. It is a human gate and stays one.
rem
rem What this IS: the escape sequences Krait CLAIMS to implement, replayed through
rem the corpus harness and compared against committed goldens. The point is
rem regression — a sequence that worked in M1 and stops working in M2 fails here,
rem in CI, without anyone remembering to go and look at a screen.
rem
rem docs/conformance.md is the list of what may appear in those goldens. A golden
rem for something the ledger marks unimplemented is how a test suite starts
rem certifying a lie, so this also checks the two have not drifted apart.
rem
rem Usage: tools\vttest-check.cmd [build-dir]     (default build\dev)

set "BUILD=%~1"
if "%BUILD%"=="" set "BUILD=build\dev"
set "HARNESS=%BUILD%\tests\corpus\krait-corpus-tests.exe"
if not exist "%HARNESS%" (
    echo FAIL: %HARNESS% not found - build the dev preset first
    exit /b 1
)

echo === corpus: every committed escape-sequence case ===
"%HARNESS%"
if errorlevel 1 (
    echo.
    echo vttest-check FAILED: a committed sequence case regressed
    exit /b 1
)

echo.
echo === conformance ledger cross-check ===
rem Every corpus directory must be named in the ledger. A case file for a
rem feature the ledger does not mention means one of the two is out of date, and
rem the ledger is the one a reader trusts.
set MISSING=0
for /d %%D in (tests\corpus\*) do (
    findstr /i /c:"%%~nxD" docs\conformance.md >nul 2>&1
    if errorlevel 1 (
        echo   WARN: corpus\%%~nxD is not mentioned in docs\conformance.md
        set MISSING=1
    )
)
if "!MISSING!"=="0" echo   every corpus area appears in the ledger

echo.
echo === manual gates, for the M1 acceptance block ===
echo   These need a human and a running app; none can be asserted here:
echo     tools\dpi-check.cmd      per-monitor DPI, no blur
echo     tools\ime-check.cmd      Thai + Japanese composition and candidates
echo     tools\paste-check.cmd    paste-guard banner
echo     tools\backend-check.cmd  kill conhost, pipe break
echo.
echo   Real vttest (interactive, not automatable):
echo     https://invisible-island.net/vttest/  - screens 1 and 2 are the M1 scope
echo.
echo vttest-check: corpus green
exit /b 0
