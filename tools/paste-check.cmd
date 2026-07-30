@echo off
setlocal
rem T28 manual gate. The sanitiser and the risk classifier are unit-tested
rem (tests/unit/paste_test.cpp); what a test cannot check is that the BANNER
rem appears, is readable, is reachable from the keyboard, and is not modal —
rem which is the part rules/ui.md actually cares about.
rem
rem Usage: tools\paste-check.cmd [path\to\krait-app.exe]

set "APP=%~1"
if "%APP%"=="" set "APP=build\dev\src\app\krait-app.exe"
if not exist "%APP%" (
    echo FAIL: %APP% not found - build the dev preset first
    exit /b 1
)
if defined QT_ROOT set "PATH=%QT_ROOT%\bin;%PATH%"

rem A payload carrying all three risks at once: an escape sequence that must be
rem stripped, a dangerous command, and more than one line.
powershell -NoProfile -Command ^
  "$t = \"echo start`e]0;pwned`asudo rm -rf /tmp/krait-demo`necho done`n\"; Set-Clipboard -Value $t"
if errorlevel 1 (
    echo FAIL: could not set the clipboard
    exit /b 1
)

echo.
echo The clipboard now holds a paste that is multiline, contains an OSC escape
echo sequence, and contains 'sudo rm -rf'.
echo.
echo Check each of these, then close the window:
echo.
echo   1. Ctrl+Shift+V shows a WARNING BANNER inside the window - not a dialog,
echo      and not something that blocks the rest of the app.
echo   2. The banner names the DANGEROUS COMMAND, not "multiline": severity is
echo      ordered, and the wrong warning sends the user to check the wrong thing.
echo   3. Esc cancels and Enter allows, without touching the mouse.
echo   4. After allowing, the shell receives ONE command per line and the window
echo      title is unchanged - the OSC sequence was stripped, not executed.
echo   5. Middle-click paste goes through the same banner. A second, unguarded
echo      paste path is how paste-guards get bypassed.
echo.
"%APP%"
echo.
echo paste-check: manual gate, record the result in STATE.md
