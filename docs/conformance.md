# VT conformance ledger

One row per sequence family. Updated in the SAME commit as the implementation
(`/add-escape-sequence` enforces the procedure). Status: ✔ full · ◐ partial
(note what's missing) · ✗ unimplemented (must be honestly reported in
DA/DECRQM) · n/a not applicable.

| Family | Sequences | Status | Tests | Notes |
|---|---|---|---|---|
| C0 controls | BEL BS HT LF CR ... | ✗ | — | M0 |
| CSI cursor | CUU CUD CUF CUB CUP HVP ... | ✗ | — | M0 |
| SGR basic | 0–29, 30–49, 90–107 | ✗ | — | M0 |
| SGR extended | 38/48 truecolor+256, 58/59, 4:x underlines | ✗ | — | M1 |
| Erase/scroll | ED EL IL DL SU SD DECSTBM | ✗ | — | M0/M1 |
| Modes | DECSET/DECRST incl. 1049, 2004, 2026, 2027, 2048 | ✗ | — | staged |
| OSC | 0/2 title, 4/10/11 colors, 7 cwd, 8 links, 52 clip, 9;4, 133, 66 | ✗ | — | staged |
| DCS | DECRQSS, XTGETTCAP, sixel, kitty gfx | ✗ | — | M5 |
| Keyboard | kitty keyboard protocol (all flags), win32-input-mode | ✗ | — | M2 |
| Reports | DA1/DA2, DSR/CPR, DECRQM | ✗ | — | honest replies rule |
