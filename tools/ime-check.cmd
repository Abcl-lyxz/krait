@echo off
setlocal
rem T29 manual gate — the scripted IME demo from docs/plan/01-milestones.md:
rem
rem   "focus Krait -> switch to Thai IME -> type สวัสดี -> composition draws at
rem    the cursor cell, commit inserts, width correct. Repeat with Japanese
rem    (へんかん -> 変換 via candidate window)."
rem
rem The measuring is unit-tested (tests/unit/ime_test.cpp, which is what a
rem metrics change re-runs per rules/render.md). What no test can check is
rem whether the candidate WINDOW lands where the composition is, because that
rem window belongs to the IME and lives outside our process.
rem
rem Usage: tools\ime-check.cmd [path\to\krait-app.exe]

set "APP=%~1"
if "%APP%"=="" set "APP=build\dev\src\app\krait-app.exe"
if not exist "%APP%" (
    echo FAIL: %APP% not found - build the dev preset first
    exit /b 1
)
if defined QT_ROOT set "PATH=%QT_ROOT%\bin;%PATH%"

echo.
echo Installed input languages:
powershell -NoProfile -Command ^
  "Get-WinUserLanguageList | ForEach-Object { '  {0}  ({1})' -f $_.LanguageTag, $_.Autonym }"
echo.
echo If Thai (th) or Japanese (ja) is missing, add it in Settings ^> Time ^&
echo language ^> Language ^& region before running this gate.
echo.
echo THAI - zero-width marks:
echo   1. Focus the terminal, switch to the Thai IME, type: Sawatdee in Thai
echo   2. The composition draws AT THE CURSOR CELL, underlined - not at the
echo      start of the line and not over the prompt.
echo   3. It occupies FOUR cells, not six: the vowel sign and the tone mark are
echo      zero width and sit over their base letters.
echo   4. Commit. The committed text occupies the same four cells the
echo      composition did - no reflow jump, no shimmer.
echo.
echo JAPANESE - double width and a candidate window:
echo   5. Switch to the Japanese IME and type henkan in kana.
echo   6. The composition occupies EIGHT cells (four characters, two cells each).
echo   7. Press space to open the candidate window. It appears UNDER THE
echo      COMPOSITION - not at the top-left of the screen, and not on another
echo      monitor.
echo   8. Convert to kanji and commit. Width stays correct.
echo.
echo DPI - the case that breaks candidate placement:
echo   9. Drag the window to a monitor at a different scale, or change the scale
echo      while it is open, and repeat step 5. The candidate window must follow
echo      the composition, not the old cell size.
echo.
"%APP%"
echo.
echo ime-check: manual gate, record the result in STATE.md
