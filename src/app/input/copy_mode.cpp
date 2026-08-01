#include "copy_mode.h"

#include "core/grid/cell.h"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace krait::app::input {
namespace {

using core::vt::ClusterPool;
using core::vt::Line;

// What kind of thing a cluster is, for the word motions. Vim's three classes,
// which is what makes `w` stop at the `/` in a path instead of skipping it.
enum class Klass : std::uint8_t { Blank, Word, Punct };

int lineWidth(const Line& line) {
    return static_cast<int>(line.cells.size());
}

// The column the cluster covering `col` STARTS at.
//
// A double-width cluster owns two columns and stores kWideTrailing in the right
// one. That cell owns no text, so it is not a position a cursor may occupy —
// every motion runs its result through here.
int startOf(const Line& line, int col) {
    if (col > 0 && col < lineWidth(line) &&
        core::vt::isWideTrailing(line.cells[static_cast<std::size_t>(col)].ch)) {
        return col - 1;
    }
    return col;
}

// Columns the cluster starting at `col` occupies: 2 when the cell to its right
// is its trailing half, 1 otherwise. Asking the GRID rather than re-measuring
// with clusterWidth() is deliberate — the grid already applied the width tables
// and the East-Asian-Ambiguous setting when it placed the cluster, and a second
// opinion here would be a second answer to disagree with.
int clusterCells(const Line& line, int col) {
    const int next = col + 1;
    return next < lineWidth(line) &&
                   core::vt::isWideTrailing(line.cells[static_cast<std::size_t>(next)].ch)
               ? 2
               : 1;
}

// The class of the cluster at `col`. The BASE codepoint decides: a combining
// mark never changes what kind of thing its cluster is, which is exactly why
// this asks the cluster and not the cell.
Klass klassAt(const Line& line, int col, const ClusterPool& clusters) {
    if (col < 0 || col >= lineWidth(line)) {
        return Klass::Blank;
    }
    const char32_t ch = line.cells[static_cast<std::size_t>(startOf(line, col))].ch;
    if (ch == 0) {
        return Klass::Blank;  // never written
    }
    char32_t base = ch;
    if (core::vt::isClusterRef(ch)) {
        const std::span<const char32_t> cluster = clusters.lookup(ch);
        if (cluster.empty()) {
            return Klass::Blank;  // a ref that outlived its pool
        }
        base = cluster.front();
    }
    if (base == U' ' || base == U'\t') {
        return Klass::Blank;
    }
    if (base >= 0x80) {
        // CJK, Thai, Cyrillic, emoji — all Word.
        //
        // There is no word segmenter here for the scripts that do not put
        // spaces between words, and pretending otherwise is how `w` lands in
        // the middle of a Thai word. Treating a run of them as ONE word is
        // coarse; landing inside one would be wrong, and this project exists
        // because that class of guess is what breaks terminals.
        return Klass::Word;
    }
    const bool word = (base >= U'0' && base <= U'9') || (base >= U'a' && base <= U'z') ||
                      (base >= U'A' && base <= U'Z') || base == U'_';
    return word ? Klass::Word : Klass::Punct;
}

// One cluster forward, crossing to the next row at the end of one. False when
// there is nowhere further to go inside the viewport.
bool stepForward(int& row, int& col, std::span<const Line> viewport) {
    const Line& line = viewport[static_cast<std::size_t>(row)];
    const int here = startOf(line, col);
    const int next = here + clusterCells(line, here);
    if (next < lineWidth(line)) {
        col = next;
        return true;
    }
    if (row + 1 >= static_cast<int>(viewport.size())) {
        return false;
    }
    ++row;
    col = 0;
    return true;
}

// One cluster back, crossing to the previous row at the start of one.
bool stepBack(int& row, int& col, std::span<const Line> viewport) {
    const Line& line = viewport[static_cast<std::size_t>(row)];
    const int here = startOf(line, col);
    if (here > 0) {
        col = startOf(line, here - 1);
        return true;
    }
    if (row == 0) {
        return false;
    }
    --row;
    const Line& above = viewport[static_cast<std::size_t>(row)];
    col = startOf(above, std::max(0, lineWidth(above) - 1));
    return true;
}

Klass klassAt(int row, int col, std::span<const Line> viewport, const ClusterPool& clusters) {
    return klassAt(viewport[static_cast<std::size_t>(row)], col, clusters);
}

// `w`: past the rest of this word, then past the blanks after it.
void wordNext(int& row, int& col, std::span<const Line> viewport, const ClusterPool& clusters) {
    const Klass from = klassAt(row, col, viewport, clusters);
    if (from != Klass::Blank) {
        while (klassAt(row, col, viewport, clusters) == from) {
            if (!stepForward(row, col, viewport)) {
                return;
            }
        }
    }
    while (klassAt(row, col, viewport, clusters) == Klass::Blank) {
        if (!stepForward(row, col, viewport)) {
            return;
        }
    }
}

// `b`: back to the first cluster of the word we are in, or of the one before.
void wordPrev(int& row, int& col, std::span<const Line> viewport, const ClusterPool& clusters) {
    if (!stepBack(row, col, viewport)) {
        return;
    }
    while (klassAt(row, col, viewport, clusters) == Klass::Blank) {
        if (!stepBack(row, col, viewport)) {
            return;
        }
    }
    const Klass from = klassAt(row, col, viewport, clusters);
    int prevRow = row;
    int prevCol = col;
    while (stepBack(prevRow, prevCol, viewport) &&
           klassAt(prevRow, prevCol, viewport, clusters) == from) {
        row = prevRow;
        col = prevCol;
    }
}

// `e`: forward to the last cluster of the word we are in, or of the next one.
void wordEnd(int& row, int& col, std::span<const Line> viewport, const ClusterPool& clusters) {
    if (!stepForward(row, col, viewport)) {
        return;
    }
    while (klassAt(row, col, viewport, clusters) == Klass::Blank) {
        if (!stepForward(row, col, viewport)) {
            return;
        }
    }
    const Klass from = klassAt(row, col, viewport, clusters);
    int nextRow = row;
    int nextCol = col;
    while (stepForward(nextRow, nextCol, viewport) &&
           klassAt(nextRow, nextCol, viewport, clusters) == from) {
        row = nextRow;
        col = nextCol;
    }
}

// The last column of a row that holds anything, so `$` lands on text rather
// than in the padding every row is allocated with.
int lastWritten(const Line& line) {
    for (int col = lineWidth(line) - 1; col >= 0; --col) {
        if (line.cells[static_cast<std::size_t>(col)].ch != 0) {
            return startOf(line, col);
        }
    }
    return 0;
}

}  // namespace

Command translateCopyKey(int key, Qt::KeyboardModifiers mods) {
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    if (ctrl) {
        // Ctrl-U and Ctrl-D only. Every other Ctrl chord falls through so the
        // palette and the tab shortcuts still work while copy mode is on.
        if (key == Qt::Key_U) {
            return {.kind = Command::Kind::Move, .motion = Motion::HalfPageUp};
        }
        if (key == Qt::Key_D) {
            return {.kind = Command::Kind::Move, .motion = Motion::HalfPageDown};
        }
        return {};
    }
    const auto move = [](Motion motion) {
        return Command{.kind = Command::Kind::Move, .motion = motion};
    };
    switch (key) {
    case Qt::Key_H:
    case Qt::Key_Left:
        return move(Motion::Left);
    case Qt::Key_J:
    case Qt::Key_Down:
        return move(Motion::Down);
    case Qt::Key_K:
    case Qt::Key_Up:
        return move(Motion::Up);
    case Qt::Key_L:
    case Qt::Key_Right:
        return move(Motion::Right);
    case Qt::Key_W:
        return move(Motion::WordNext);
    case Qt::Key_B:
        return move(Motion::WordPrev);
    case Qt::Key_E:
        return move(Motion::WordEnd);
    case Qt::Key_0:
    case Qt::Key_Home:
        return move(Motion::LineStart);
    case Qt::Key_Dollar:
    case Qt::Key_End:
        return move(Motion::LineEnd);
    case Qt::Key_PageUp:
        return move(Motion::HalfPageUp);
    case Qt::Key_PageDown:
        return move(Motion::HalfPageDown);
    case Qt::Key_G:
        // Shift tells the two apart, and Qt reports Key_G for both. One `g`
        // rather than vim's `gg`: copy mode has no count prefix to disambiguate
        // it from, so the second keystroke would buy nothing.
        return move(shift ? Motion::Bottom : Motion::Top);
    case Qt::Key_V:
        return {.kind = Command::Kind::Select, .select = shift ? Selecting::Line : Selecting::Char};
    case Qt::Key_Y:
        return {.kind = Command::Kind::Yank};
    case Qt::Key_Escape:
        return {.kind = Command::Kind::Leave};
    default:
        return {};
    }
}

int applyMotion(CopyCursor& cursor, Motion motion, std::span<const core::vt::Line> viewport,
                const ClusterPool& clusters) {
    if (viewport.empty()) {
        return 0;
    }
    const int lastRow = static_cast<int>(viewport.size()) - 1;
    const int half = std::max(1, static_cast<int>(viewport.size()) / 2);
    cursor.row = std::clamp(cursor.row, 0, lastRow);
    int scroll = 0;

    switch (motion) {
    case Motion::Left: {
        const Line& line = viewport[static_cast<std::size_t>(cursor.row)];
        const int here = startOf(line, cursor.col);
        cursor.col = here > 0 ? startOf(line, here - 1) : 0;
        break;
    }
    case Motion::Right: {
        const Line& line = viewport[static_cast<std::size_t>(cursor.row)];
        const int here = startOf(line, cursor.col);
        const int next = here + clusterCells(line, here);
        cursor.col = next < lineWidth(line) ? next : here;
        break;
    }
    case Motion::Up:
        if (cursor.row > 0) {
            --cursor.row;
        } else {
            scroll = 1;  // already at the top row: the VIEWPORT moves instead
        }
        break;
    case Motion::Down:
        if (cursor.row < lastRow) {
            ++cursor.row;
        } else {
            scroll = -1;
        }
        break;
    case Motion::WordNext:
        wordNext(cursor.row, cursor.col, viewport, clusters);
        break;
    case Motion::WordPrev:
        wordPrev(cursor.row, cursor.col, viewport, clusters);
        break;
    case Motion::WordEnd:
        wordEnd(cursor.row, cursor.col, viewport, clusters);
        break;
    case Motion::LineStart:
        cursor.col = 0;
        break;
    case Motion::LineEnd:
        cursor.col = lastWritten(viewport[static_cast<std::size_t>(cursor.row)]);
        break;
    case Motion::HalfPageUp:
        scroll = half;
        break;
    case Motion::HalfPageDown:
        scroll = -half;
        break;
    case Motion::Top:
        // Grid::scrollView clamps to what history can supply, so asking for
        // more than exists is the whole implementation of "as far back as it
        // goes" — there is no public maxViewOffset() to ask.
        scroll = std::numeric_limits<int>::max() / 2;
        cursor.row = 0;
        cursor.col = 0;
        break;
    case Motion::Bottom:
        scroll = std::numeric_limits<int>::min() / 2;
        cursor.row = lastRow;
        cursor.col = 0;
        break;
    }

    // THE INVARIANT. A vertical or page motion keeps the column while the row
    // changes under it, so the column that was a cluster start on the old row
    // can be the trailing half of a wide cluster on the new one. Snapping here,
    // once, is what keeps every motion out of the middle of a cluster — doing
    // it per-arm above is how one of them eventually gets forgotten.
    const Line& landed = viewport[static_cast<std::size_t>(cursor.row)];
    cursor.col = std::clamp(cursor.col, 0, std::max(0, lineWidth(landed) - 1));
    cursor.col = startOf(landed, cursor.col);

    if (cursor.select == Selecting::Off) {
        cursor.anchorRow = cursor.row;
        cursor.anchorCol = cursor.col;
    }
    return scroll;
}

}  // namespace krait::app::input
