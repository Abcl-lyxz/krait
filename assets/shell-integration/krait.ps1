# Krait shell integration for PowerShell — OSC 133 prompt marks.
#
# Dot-source it, or let Krait install it (file panel -> Shell integration),
# which writes this file between two markers in your $PROFILE.
#
# WHAT IS EMITTED: A (prompt start), B (input start) and D;<status>.
#
# NOT C. C says "the command is now running" and PowerShell has no hook between
# Enter and the command — the only way to reach that point is to take over
# PSReadLine's Enter handler, which is the user's, not ours. Microsoft's own
# shell-integration snippet for Windows Terminal emits A, B and D for exactly
# this reason and no C either. The cost is Krait's long-command notification,
# which is armed by C and therefore does not fire for remote PowerShell.
#
# No interactive test is needed: PowerShell only ever CALLS `prompt` in an
# interactive session, so a `pwsh -Command` run that loads this profile defines
# a function nothing invokes.

if (-not $Global:__KraitShellIntegration) {
    $Global:__KraitShellIntegration = $true

    # [char]27, never `e: the backtick-e escape arrived in PowerShell 6 and
    # Windows still ships 5.1 in the box. A script that assumes 6 prints the
    # literal text "`e]133;A" into the user's prompt on 5.1.
    $Global:__KraitEsc = [char]27
    $Global:__KraitBel = [char]7
    $Global:__KraitLastHistoryId = -1

    # The prompt that was in effect, kept WHOLE and invoked later. Captured
    # through $function:prompt rather than re-parsed out of Get-Command text, so
    # a prompt that closes over state (oh-my-posh, starship) still works.
    $Global:__KraitOriginalPrompt = $function:prompt

    function Global:prompt {
        # FIRST STATEMENT. $? is a BOOLEAN — "did the last statement succeed" —
        # and $LASTEXITCODE is the exit code of the last NATIVE executable,
        # which a failing cmdlet does not touch. Reading $LASTEXITCODE
        # unconditionally reports the exit code of some earlier command, which
        # is worse than reporting nothing: the user reads "137" and goes looking
        # for a kill that happened ten commands ago.
        $krOk = $?
        $krHistory = Get-History -Count 1
        $krCode = 0
        if (-not $krOk) {
            if ($null -ne $krHistory -and $null -ne $Error[0] -and
                $Error[0].InvocationInfo.HistoryId -eq $krHistory.Id) {
                # A PowerShell-level error. It has no exit code at all, so 1 is
                # a CONVENTION here — what a POSIX shell reports for a failed
                # builtin — and not a number PowerShell produced. It is sent
                # because "this failed" is the thing the mark exists to say.
                $krCode = 1
            } elseif ($null -ne $LASTEXITCODE) {
                $krCode = $LASTEXITCODE
            } else {
                $krCode = 1
            }
        }

        $esc = $Global:__KraitEsc
        $bel = $Global:__KraitBel
        $out = ''
        # Nothing has run yet on the very first prompt, and neither has anything
        # when Ctrl+C or a bare Enter leaves the history id where it was. A D
        # with an invented 0 in either case is a green mark for a command that
        # never existed.
        if ($Global:__KraitLastHistoryId -ne -1 -and
            $null -ne $krHistory -and $krHistory.Id -ne $Global:__KraitLastHistoryId) {
            $out += "$esc]133;D;$krCode$bel"
        }
        $out += "$esc]133;A$bel"
        $out += $Global:__KraitOriginalPrompt.Invoke()
        $out += "$esc]133;B$bel"
        if ($null -ne $krHistory) {
            $Global:__KraitLastHistoryId = $krHistory.Id
        }
        return $out
    }
}
