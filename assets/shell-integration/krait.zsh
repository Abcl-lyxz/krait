# Krait shell integration for zsh — OSC 133 prompt marks.
#
# Source it, or let Krait install it (file panel -> Shell integration), which
# writes this file between two markers in ~/.zshrc.
#
# WHAT IS EMITTED: see krait.bash. Krait's parser reads A, P, B, C, D and `k=`
# and nothing else.
#
# Guarded by an `if` rather than an early `return`, because this block lives
# INSIDE the user's .zshrc and a `return` would skip the rest of their file.

if [[ -o interactive ]] && [[ -z ${KRAIT_SHELL_INTEGRATION-} ]]; then
    KRAIT_SHELL_INTEGRATION=1

    autoload -Uz add-zsh-hook

    __krait_precmd() {
        # FIRST LINE. `cmd_status`, NOT `status`: in zsh `status` is a synonym
        # for `?`, so declaring it local is a way to destroy the value being
        # read.
        local -i cmd_status=$?
        print -n -- "\e]133;D;${cmd_status}\a\e]133;A\a"
    }

    __krait_preexec() {
        print -n -- "\e]133;C\a"
    }

    # add-zsh-hook is idempotent by function NAME — its implementation skips a
    # name already in the array — so the guard above is belt and braces rather
    # than the only thing standing between a re-source and doubled marks.
    add-zsh-hook precmd __krait_precmd
    add-zsh-hook preexec __krait_preexec

    # MOVED TO THE FRONT, for exactly the reason bash's PROMPT_COMMAND entry is
    # prepended: zsh runs precmd_functions in order, and only the first one
    # still sees the COMMAND's $? — every later one sees the previous hook's.
    # add-zsh-hook appends, so under oh-my-zsh, powerlevel10k or vcs_info, which
    # all register a precmd before a user's rc file gets here, D would otherwise
    # report the exit status of somebody else's prompt hook.
    #
    # ${array:#pattern} drops matching elements, so this stays idempotent.
    precmd_functions=(__krait_precmd ${precmd_functions:#__krait_precmd})

    # %{ %} is zsh's "these bytes occupy no columns", the job \[ \] does in
    # bash. Without it a wrapped command line is drawn in the wrong place.
    PS1="$PS1"$'%{\e]133;B\a%}'
    # k=s: a continuation prompt is not somewhere jump-to-prompt should land,
    # and a D arriving after one must attach to the line the command started on.
    PS2=$'%{\e]133;A;k=s\a%}'"$PS2"
fi
