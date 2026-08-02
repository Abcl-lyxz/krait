#pragma once

#include "core/grid/cell.h"
#include "core/grid/cluster_pool.h"
#include "core/grid/damage.h"
#include "core/grid/images.h"
#include "core/grid/line.h"
#include "core/grid/scrollback.h"
#include "core/grid/sync_output.h"
// State only, no handlers: kitty_keys.h forward-declares Grid rather than
// including it, so this direction does not close a cycle.
#include "core/parser/kitty_keys.h"
#include "core/unicode/width.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace krait::core::vt {

// One DECSC / mode-1049 saved-cursor slot. xterm's SavedCursor (cursor.c
// CursorSave2) keeps position, SGR attributes, origin mode, the pending-wrap
// flag and charset state — but NOT the scrolling margins, which are screen
// state rather than cursor state (DEC agrees; see vt100.net DECSC).
struct SavedCursor {
    int row = 0;  // ABSOLUTE, even when origin mode was on at save time
    int col = 0;
    Attr pen;
    bool originMode = false;
    bool pendingWrap = false;
    bool g1Invoked = false;
};

// Screen + scrollback (T8). Replaces the T6/T7 StubGrid: same open members
// the CSI handlers mutate, but with real wrap, scroll, damage, and resize.
// DECAWM (autowrap) is always on for now, with DEC deferred-wrap semantics:
// writing the last column leaves the cursor there and sets pendingWrap; the
// NEXT printable wraps. Cursor movement clears pendingWrap.
class Grid {
  public:
    Grid(int rowCount, int colCount);

    int rows;  // visual screen size
    int cols;
    int row = 0;  // cursor, 0-based
    int col = 0;
    bool pendingWrap = false;
    bool g1Invoked = false;  // SO/SI shift state; charsets are later work
    int bells = 0;
    Attr pen;
    DamageList damage;

    // East-Asian-Ambiguous resolution for every width this grid measures.
    // There is no right default, only a per-session setting (width.h): T31
    // wires it to the settings registry and mode 2027 is how an application
    // negotiates it. Never guess per-codepoint at a call site.
    unicode::Ambiguous ambiguous = unicode::Ambiguous::Narrow;

    // Mode 2004, bracketed paste. Stored here and reported by DECRQM; the
    // wrapping of a paste in ESC[200~ / ESC[201~ is the input path's job
    // (T28), which is also where the paste-guard lives.
    bool bracketedPaste = false;

    // Mode 1, DECCKM. Changes what the arrow keys SEND (CSI A vs SS3 A), so
    // like bracketedPaste it is state the core owns and the input path reads.
    // It never changes what the grid draws.
    bool appCursorKeys = false;

    // Mouse reporting (T27). The three tracking modes are mutually exclusive in
    // xterm — 1002 supersedes 1000, and resetting whichever is active is what
    // turns tracking off — so one enum, not three bools. Three bools is how
    // terminals end up still reporting motion after an app disabled only 1003.
    enum class MouseTracking : std::uint8_t {
        Off,
        Normal,       // 1000: press and release only
        ButtonEvent,  // 1002: press, release, and motion while a button is down
        AnyEvent,     // 1003: press, release, and every motion
    };
    MouseTracking mouseTracking = MouseTracking::Off;

    // Mode 1006, SGR encoding. The default X10 encoding cannot express a column
    // past 222, so on any wider window it silently reports the wrong cell —
    // which is why every modern application asks for 1006.
    bool sgrMouse = false;

    // Kitty keyboard protocol (T48). Same shape as the three above: state the
    // core owns and the INPUT path reads, which never changes what is drawn.
    KittyKeyboard kittyKeys;

    // Mode 2026, synchronized output. See sync_output.h for why the guard's
    // clock is supplied by the caller rather than read here.
    SyncOutput sync;

    // Mode 2048, in-band resize notification (T82). Same shape as the input
    // modes above: state the core owns, read by whoever handles the resize.
    //
    // The spec is explicit that enabling it MUST immediately report the current
    // size — an application turns it on precisely so it can stop guessing, and
    // making it wait for the next resize means it guesses once more.
    bool inBandResize = false;

    // Mode 2031, colour-palette update notification (T83). The terminal reports
    // light-versus-dark when the THEME changes, which is a fact only the app
    // layer has — so the core owns the switch and never the notification.
    bool paletteUpdates = false;

    // Sixel and kitty graphics (M5 T79/T80). On the Grid rather than beside it
    // for the same reason OSC 133's marks are on the Line: placements are
    // anchored into scrollback's stable index space, and the only thing that
    // knows when a line is evicted is the thing that evicted it.
    ImageStore images;

    // Cell size in device pixels, supplied by the renderer. src/core/ has no
    // font and no window (rules/vt-core.md), so this is the only way a reply
    // that must carry pixels can carry honest ones — mode 2048's report is the
    // first. Left at 0 the pixel fields report 0, which the spec allows in as
    // many words ("or 0 if not supported").
    int cellWidthPx = 0;
    int cellHeightPx = 0;

    // The integrator's monotonic millisecond stamp, refreshed before each fed
    // chunk. src/core/ reads no clock (rules/vt-core.md: zero OS deps, and
    // every test stays deterministic), so this is how a DECSET 2026 arriving
    // mid-stream learns when "now" is. Left at 0 it simply means the 2026
    // guard measures in chunks the caller never stamped — the mode still
    // works, the timeout just cannot fire.
    std::uint64_t nowMs = 0;

    // Scrolling region (DECSTBM), 0-based and INCLUSIVE. Defaults to the whole
    // screen. DEC VT510: "You cannot perform scrolling outside the margins."
    int scrollTop = 0;
    int scrollBottom;         // rows - 1 at construction
    bool originMode = false;  // DECOM: addressing is relative to scrollTop

    bool inScrollRegion(int r) const { return r >= scrollTop && r <= scrollBottom; }

    // Whether lines scrolled off the top belong in scrollback. xterm's gate is
    // the TOP margin alone — `scroll_all_lines = (scrollWidget && !whichBuf &&
    // screen->top_marg == 0)` — NOT a full-screen region. An app reserving a
    // footer (top=0, bottom<rows-1) still wants its history kept; one that
    // pins a header (top>0) is managing its own viewport and must not pollute
    // history. The alternate screen never captures at all — full-screen apps
    // own their viewport and their redraws are not history — EXCEPT that a
    // row-shrinking resize() retires active-buffer rows unconditionally, which
    // xterm also does (ScreenResize gates that copy on gravity, not whichBuf).
    bool capturesScrollback() const { return scrollTop == 0 && !m_onAlt; }

    // Scroll the region by n lines. Lines pushed out of the region are lost
    // (DEC: "Lines scrolled off the page are lost"), except off the top of a
    // full-screen region, which goes to scrollback. Blank lines carry no
    // attributes. n is clamped to the region height.
    void scrollRegionUp(int n);
    void scrollRegionDown(int n);

    // The ONE cursor-addressing path: CUP, HVP, VPA, DECSTBM's home and
    // DECRC all route here so origin mode is applied in exactly one place.
    // `r`/`c` are 0-based. With origin mode on, `r` is relative to scrollTop
    // and the result cannot leave the region; with it off, `r` addresses the
    // whole page (xterm cursor.c CursorSet).
    void cursorSet(int r, int c);

    // Alternate screen (mode 1049). Only the CELLS swap: in xterm the margins,
    // pen and cursor are single TScreen members and only `whichBuf` selects
    // between two cell arrays — so a region set on the normal screen is still
    // in effect on the alternate one.
    void useAlternateScreen(bool on);

    bool onAlternateScreen() const { return m_onAlt; }

    // Blanks every row of the ACTIVE buffer (mode 1049's "clearing it first").
    //
    // This and useAlternateScreen() allocate, and every CSI handler that calls
    // them is `noexcept` — a settled decision, not an oversight: a bad_alloc
    // while growing one screen's worth of cells means the process is already
    // finished, and there is no per-sequence recovery worth writing. The whole
    // handler family (handleErase, handleScroll, handleMode) is noexcept on the
    // same grounds. Revisit only if the grid ever becomes unbounded.
    void eraseScreen();

    // Two slots, indexed by which buffer is active. 1049 saves BEFORE it
    // switches, so its save lands in the normal screen's slot and clobbers a
    // pending ESC 7 — exactly what xterm's sc[whichBuf] does.
    void saveCursor();
    void restoreCursor();

    Cell& cellAt(int r, int c);
    const Cell& cellAt(int r, int c) const;
    Line& lineAt(int r);
    const Line& lineAt(int r) const;

    // Writes at the cursor with the current pen, honoring deferred wrap.
    //
    // Takes ONE codepoint because that is what the parser produces, but the
    // unit it stores is a grapheme CLUSTER (rules/vt-core.md). A codepoint that
    // continues the cluster already under the cursor — a combining mark, a
    // variation selector, a ZWJ join, the second half of a flag — extends that
    // cell instead of claiming a new one, and a 2-column cluster claims a
    // second cell marked kWideTrailing. Both are why the cursor can advance by
    // 0, 1 or 2 columns for a single call.
    void putChar(char32_t ch);

    // The table behind cells whose ch carries kClusterTag. Exposed so the
    // renderer (and tests) can resolve a cell back to its codepoints without
    // this class having to hand out spans with a lifetime attached.
    const ClusterPool& clusters() const { return m_clusters; }

    // Cursor down one row; scrolls (into scrollback) at the bottom.
    void linefeed();

    std::size_t scrollbackSize() const { return m_scrollback.lineCount(); }

    const Line& scrollbackAt(std::size_t i) const { return m_scrollback.lineAt(i); }

    // History, for the viewport and for whoever sets the per-tab cap (T31).
    const Scrollback& scrollback() const { return m_scrollback; }

    Scrollback& scrollback() { return m_scrollback; }

    // How far the viewport is scrolled UP into history, in visual rows. 0 is
    // the live screen. Clamped to what history can actually supply, so a wheel
    // spin at the top is a no-op rather than a blank screen.
    int viewOffset() const { return m_viewOffset; }

    // Scrolls the viewport by `delta` rows (positive = back into history) and
    // returns whether it actually moved. Damage is marked only when it did:
    // a no-op scroll must not cost a full repaint every wheel tick.
    bool scrollView(int delta);

    // Snaps the viewport back to the live screen. Plain output does NOT call
    // this: a reader scrolled up keeps their place, and pushToScrollback
    // follows the content so it does not shift under them. It is for the input
    // path (T27: a keypress snaps to the bottom, as every terminal does).
    // Anything that invalidates row offsets wholesale — a buffer swap, a
    // resize — resets the offset directly rather than through here.
    void scrollViewToBottom();

    // The rows to DRAW, top to bottom: history rows first when scrolled up,
    // then as much of the live screen as still fits.
    std::vector<Line> viewportRows() const;

    // --- OSC 133 shell integration (T66) ---
    //
    // Marks live on the Line (line.h), never in a table keyed by row number.
    // The calls below address lines as scrollback logical lines first
    // (0 .. scrollbackSize()-1), then SCREEN rows.
    //
    // That is the same shape as SearchHit::line but NOT the same space: search
    // enumerates `viewportRows()`, which serves rewrapped HISTORY rows whenever
    // the viewport is scrolled up, while this always ends in m_screen. The two
    // agree only at viewOffset() == 0. Said plainly here because a UI that
    // assumed one space and got the other would scroll to the wrong line
    // exactly when the user is scrolled back — which is when a jump-to-prompt
    // binding is most likely to be used.
    //
    // Marks are NORMAL-buffer only: markPrompt/setCommandExit are no-ops while
    // the alternate screen is up, since m_scrollback is the normal buffer's
    // history and splicing the two would name lines that do not exist.
    std::size_t absoluteLineCount() const { return m_scrollback.lineCount() + m_screen.size(); }

    // Precondition: i < absoluteLineCount(). Unchecked, like cellAt/lineAt —
    // and an index does NOT survive a feed() or a resize, so re-derive it from
    // prevPrompt/nextPrompt rather than caching one across either.
    const Line& absoluteLineAt(std::size_t i) const;

    // Applies mark bits to the HEAD row of the logical line under the cursor.
    // The head, not the cursor's row: a prompt long enough to wrap is one
    // logical line, and reflow re-attaches marks to whichever row heads the
    // run — so a mark parked on a continuation row would jump to the top of
    // the prompt on the next resize instead of staying where it was put.
    void markPrompt(std::uint8_t bits);

    // OSC 133 ; D ; <n>. Writes the status onto the NEWEST line carrying
    // kMarkPromptStart at or after the open prompt's floor — the line the user
    // typed the command on, which by now is usually in scrollback. Deliberately
    // not "at or above the cursor": a shell may move the cursor above its own
    // prompt between `A` and `D` (zsh transient prompts do), and anchoring the
    // walk there attributed the status to the previous command. A negative
    // code, or a D with no prompt open, is a no-op — and the walk it does
    // perform is bounded by where the prompt was opened, never by the size of
    // history (see the comment on the implementation).
    void setCommandExit(int code);

    // Nearest kMarkPromptStart STRICTLY before / after `from`, in the index
    // space above. Strictly, because a jump-to-prompt binding pressed twice
    // has to move twice.
    //
    // `floor` is the OLDEST index worth looking at, inclusive. Default 0 for
    // the UI, which is answering a keypress and may search everything; the
    // parse thread passes a real floor so a remote stream cannot buy a
    // full-history scan for 25 bytes (setCommandExit).
    std::optional<std::size_t> prevPrompt(std::size_t from, std::size_t floor = 0) const;
    std::optional<std::size_t> nextPrompt(std::size_t from) const;

    // The absolute line the TOP of the viewport is currently showing — the
    // anchor prevPrompt/nextPrompt should be asked about, so that pressing the
    // binding twice walks two prompts back rather than returning to the same
    // one. Equal to scrollbackSize() while the viewport is at the bottom.
    std::size_t viewTopLine() const;

    // Scrolls so the absolute line `i` is the viewport's TOP row. A line on the
    // live screen snaps the viewport to the bottom instead — the screen is
    // always on show, and scrolling history over it would hide the thing asked
    // for. Marks damage only when the view actually moved.
    void scrollToLine(std::size_t i);

    // Rows: shrinking retires lines off the TOP (into scrollback for the
    // active buffer), growth appends blank rows at the bottom.
    //
    // Columns: the NORMAL screen is rewrapped (reflow.h) — logical lines are
    // rejoined and re-split, and the cursor rides along. The alternate screen
    // is only truncated/padded: a full-screen application owns that viewport
    // and redraws it on SIGWINCH, so rewrapping it would fight the redraw.
    void resize(int newRows, int newCols);

  private:
    // Retires a line off the top of the screen into scrollback.
    void pushToScrollback(Line&& line);

    // Furthest the viewport may scroll back, in visual rows. ONE clamp for
    // every mover — the wheel, a jump to a prompt, and history arriving under a
    // scrolled-back viewport all pass through it.
    int maxViewOffset() const;

    // putChar's three phases, split out because the wrap rules differ between
    // starting a cluster and extending one.
    void wrapToNextRow();
    void beginCluster(char32_t ch);
    void appendToCluster(char32_t ch);
    void advanceCursor(int width);
    // Blanks the orphaned half of any 2-column cluster that the span
    // [first, last] on row `r` is about to overwrite. Must run BEFORE the write.
    void breakWidePairs(int r, int first, int last);

    // Forgets which cell the current cluster lives in, without disturbing the
    // break state. Anything that moves CONTENT under a settled cursor must call
    // this — a scroll, an erase, a buffer swap — or a following combining mark
    // lands on whatever moved into that cell.
    void resetClusterAnchor();

    // Bound on one cell's cluster. Hostile input can emit combining marks
    // forever; past this the tail is dropped rather than allowed to grow a
    // per-cell allocation without limit. Well past any real Thai syllable or
    // ZWJ emoji sequence.
    static constexpr std::size_t kMaxClusterLen = 16;

    std::vector<Line> m_screen;
    Scrollback m_scrollback;
    int m_viewOffset = 0;
    // The inactive buffer. Empty until mode 1049 is first set; from then on it
    // holds whichever buffer is NOT on screen (m_screen and this are swapped).
    std::vector<Line> m_altScreen;
    std::array<SavedCursor, 2> m_saved{};  // [0] normal, [1] alternate
    bool m_onAlt = false;

    ClusterPool m_clusters;
    unicode::ClusterBreaker m_breaker;

    // The cluster currently being built, and the cell that owns it.
    std::array<char32_t, kMaxClusterLen> m_cluster{};
    std::size_t m_clusterLen = 0;
    // What we last STORED in the anchored cell. The cursor check below cannot
    // see ED/EL: they rewrite cells without moving the cursor, so the position
    // triple still matches and a following mark would resurrect an erased cell.
    // Re-reading the cell and comparing is the check that actually holds.
    char32_t m_clusterCh = 0;
    int m_clusterRow = -1;
    int m_clusterCol = -1;

    // Where putChar left the cursor last time. A cluster cannot span a cursor
    // jump, and rather than reset the breaker at every site that moves the
    // cursor — CUP, CR, BS, tabs, DECRC, and every one added later — putChar
    // notices the cursor is no longer where it left it. Self-healing, and
    // impossible for a new sequence handler to forget.
    int m_lastRow = -1;
    int m_lastCol = -1;
    bool m_lastWrap = false;

    // The open OSC 133 prompt — one a D has not closed yet — as a FLOOR in
    // Scrollback's stable numbering (scrollback.h), or nullopt when no prompt
    // is open. Both halves are load-bearing:
    //
    //   * the optional bounds a stream of bare D's, which is what T66 shipped;
    //   * the value bounds a stream of A+D PAIRS, which T66 did not. `A` then
    //     `CSI 2J` then `D` is 25 bytes, and the 2J clears the mark (sgr.cpp)
    //     while leaving the prompt open — so a walk bounded only by "is one
    //     open" searches all of history, finds nothing, and does it again for
    //     every 25 bytes the remote sends.
    //
    // A stable index rather than a row index because a row index is precisely
    // what eviction, scrolling and reflow invalidate. It is deliberately only
    // a FLOOR, not the prompt's own index: when `A` arrives its line is still
    // on the screen and has no stable index yet, but it can only ever land on
    // or after the newest line already in history, and a floor is all a bound
    // needs. The walk is then proportional to lines pushed since `A` — work
    // the remote already paid for in bytes — plus at most the screen.
    std::optional<std::uint64_t> m_openPrompt;
};

}  // namespace krait::core::vt
