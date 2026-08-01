# Krait shell integration for fish — OSC 133 prompt marks.
#
# Source it, or let Krait install it (file panel -> Shell integration), which
# writes this file between two markers in ~/.config/fish/config.fish.
#
# WHAT IS EMITTED: A (prompt start), C (command started) and D;<status>.
#
# NOT B. B marks where the prompt ends and typing begins, and reaching that
# point in fish means replacing the user's `fish_prompt` function — the one
# thing every fish prompt framework also owns. Krait uses B for nothing today
# (it is a line flag with no consumer), so the trade is a mark nobody reads
# against a prompt that might stop working, and it is not close.
#
# Guarded by `if` and NOT by `exit`: this block lives inside the user's
# config.fish, and `exit` in a sourced file stops the rest of it from running.

if status is-interactive; and not set -q KRAIT_SHELL_INTEGRATION
    set -g KRAIT_SHELL_INTEGRATION 1

    # --on-event handlers, not a wrapped fish_prompt: fish raises these by name
    # and re-defining a function with the same name replaces it, so sourcing
    # this file twice cannot double the marks.
    function __krait_prompt --on-event fish_prompt
        printf '\033]133;A\007'
    end

    function __krait_preexec --on-event fish_preexec
        printf '\033]133;C\007'
    end

    function __krait_postexec --on-event fish_postexec
        # FIRST LINE: $status here is the exit status of the command that just
        # ran, and any command in this function replaces it — including the
        # printf that is about to report it.
        set -l cmd_status $status
        printf '\033]133;D;%s\007' $cmd_status
    end
end
