# Krait shell integration for bash — OSC 133 prompt marks.
#
# Source it, or let Krait install it (file panel -> Shell integration), which
# writes this file between two markers in ~/.bashrc.
#
# WHAT IS EMITTED, and only this: Krait's parser reads A, P, B, C, D and the
# `k=` kind parameter and nothing else (src/core/parser/osc.cpp), so sending
# anything else would be noise on the wire for a terminal that ignores it.
#   A          the prompt starts here  -> what jump-to-prompt lands on
#   A;k=s      a CONTINUATION prompt   -> deliberately NOT a jump target
#   B          the prompt ends, input starts
#   C          the command starts running -> arms the long-command notification
#   D;<status> the command ended, with its exit status
#
# NO DEBUG TRAP. bash 4.4's PS0 is expanded after a command line is read and
# before it runs, which is precisely the C mark — so the user's own DEBUG trap
# is left completely alone instead of being chained onto. Microsoft's own bash
# snippet for Windows Terminal and kitty's shell integration both do it this
# way.
#
# Guarded by an `if` rather than an early `return`, because this block lives
# INSIDE the user's .bashrc: a `return` here would silently skip the rest of
# their file.

if [[ $- == *i* && -z ${KRAIT_SHELL_INTEGRATION-} ]]; then
    KRAIT_SHELL_INTEGRATION=1

    __krait_command_end() {
        # FIRST LINE, and it has to be: $? is the exit status of the command
        # that just finished, and any command run before this one replaces it.
        local __krait_status=$?
        printf '\033]133;D;%s\007' "$__krait_status"
    }

    # PREPENDED, never appended. PROMPT_COMMAND entries run in order, and only
    # the first one still sees the command's $? — appending would report the
    # status of whatever the user's own prompt hook did last.
    #
    # Three branches because PROMPT_COMMAND is three different things: unset, a
    # string, or (bash 5.1 and later) an array. Treating an array as a string
    # turns a working prompt into an error message on every line.
    if [[ -z ${PROMPT_COMMAND[*]-} ]]; then
        PROMPT_COMMAND='__krait_command_end'
    elif [[ $(declare -p PROMPT_COMMAND 2>/dev/null) == 'declare -a'* ]]; then
        PROMPT_COMMAND=('__krait_command_end' "${PROMPT_COMMAND[@]}")
    else
        PROMPT_COMMAND=$'__krait_command_end\n'$PROMPT_COMMAND
    fi

    # \[ \] around every escape. Without them readline counts these bytes as
    # printable, and every command long enough to wrap is drawn in the wrong
    # place for the rest of the session.
    PS1='\[\033]133;A\007\]'"$PS1"'\[\033]133;B\007\]'
    PS0='\[\033]133;C\007\]'"$PS0"
    # k=s marks a secondary prompt. Without it every continuation row of a
    # multi-line command is a jump target, and worse, D's exit status attaches
    # to the LAST row instead of the line the command began on.
    PS2='\[\033]133;A;k=s\007\]'"$PS2"
fi
