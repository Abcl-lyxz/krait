@echo off
setlocal
rem T33 manual gate — the kill-conhost and pipe-break cases.
rem
rem The code-to-banner contract is unit-tested (tests/unit/error_banner_test.cpp),
rem including that PeerClosed produces banner properties and cannot become a
rem dialog. What is NOT automated is the live path: a real pseudoconsole dying
rem under a real session. That needs a running app, a real ConPTY and a kill from
rem outside, so it is a human gate and is recorded as one in STATE.md.
rem
rem Usage: tools\backend-check.cmd [path\to\krait-app.exe]

set "APP=%~1"
if "%APP%"=="" set "APP=build\dev\src\app\krait-app.exe"
if not exist "%APP%" (
    echo FAIL: %APP% not found - build the dev preset first
    exit /b 1
)
if defined QT_ROOT set "PATH=%QT_ROOT%\bin;%PATH%"

echo.
echo Krait is starting. Leave it open and, in ANOTHER window, run:
echo.
echo   1. CLEAN EXIT - type `exit` in Krait.
echo      Expected: the shell exits and NO banner appears. A clean exit is not
echo      an error, and a banner here is how banners get ignored.
echo.
echo   2. KILL THE CONSOLE HOST:
echo        taskkill /IM OpenConsole.exe /F
echo      Expected: an ERROR banner inside the tab reading "The session ended
echo      unexpectedly", with the console-host hint. NOT a message box, and NOT
echo      something that blocks the rest of the app.
echo.
echo   3. NON-ZERO EXIT - type `exit 3` in Krait.
echo      Expected: a banner naming exit code 3.
echo.
echo   4. In every case: Esc dismisses the banner and the tab stays usable.
echo.
"%APP%"
echo.
echo backend-check: manual gate, record the result in STATE.md
