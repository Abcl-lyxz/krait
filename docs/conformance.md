# VT conformance ledger

One row per sequence family. Updated in the SAME commit as the implementation
(`/add-escape-sequence` enforces the procedure). Status: ✔ full · ◐ partial
(note what's missing) · ✗ unimplemented (must be honestly reported in
DA/DECRQM) · n/a not applicable.

| Family | Sequences | Status | Tests | Notes |
|---|---|---|---|---|
| C0 controls | BEL BS HT LF CR SO SI | ◐ | corpus/csi/c0.case | T6 stub grid: BEL counted (app bell later), HT fixed stops every 8 (no HTS/TBC), LF scrolls into scrollback at the bottom (T8; no LNM), autowrap always on with DEC deferred-wrap semantics (DECAWM mode switch not implemented), SO/SI shift state only (no SCS charsets). NUL ENQ VT FF still ✗ |
| CSI cursor | CUU CUD CUF CUB CUP HVP CHA VPA | ◐ | corpus/csi/cursor.case | T6: defaults=1, 0→1, clamps at grid edges; colon subparams and intermediates/markers reject the sequence (SGR-only per xterm); no margins (DECSTBM/DECSLRM), no DECOM — those flip this to ✔ |
| SGR basic | 0–29, 30–49, 90–107 | ◐ | corpus/sgr/basic.case | T7: flags+16 colors; 21 approximated as single underline, 4:x styles collapse to on/off; 6/10-20/26 ignored. 38/48/58 consumed with correct arity (colon+legacy) but apply nothing — see SGR extended |
| SGR extended | 38/48 truecolor+256, 58/59, 4:x underlines | ✗ | — | M1 |
| Erase/scroll | ED EL IL DL SU SD DECSTBM | ◐ | corpus/sgr/erase.case | T7: ED 0/1/2 + EL 0/1/2 on stub grid (ED3 no-op until real grid; BCE undecided; DECSED/DECSEL rejected). IL DL SU SD DECSTBM still ✗ |
| Modes | DECSET/DECRST incl. 1049, 2004, 2026, 2027, 2048 | ✗ | — | staged |
| OSC | 0/2 title, 4/10/11 colors, 7 cwd, 8 links, 52 clip, 9;4, 133, 66 | ✗ | — | staged |
| DCS | DECRQSS, XTGETTCAP, sixel, kitty gfx | ✗ | — | M5 |
| Keyboard | kitty keyboard protocol (all flags), win32-input-mode | ✗ | — | M2 |
| Reports | DA1/DA2, DSR/CPR, DECRQM | ✗ | — | honest replies rule |
