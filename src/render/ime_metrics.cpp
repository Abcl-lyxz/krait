#include "render/ime_metrics.h"

#include <algorithm>

namespace krait::render {

int preeditCells(int col, int columns, int cols) {
    if (columns <= 0 || cols <= 0) {
        return 0;
    }
    const int start = std::clamp(col, 0, std::max(0, cols - 1));
    // A composition longer than the room left on the row is truncated, not
    // wrapped: the grid does not own these cells, so there is no next row to
    // continue onto, and drawing past the edge paints over the chrome.
    return std::clamp(columns, 0, cols - start);
}

CellRect cursorRect(const FaceMetrics& metrics, int row, int col, int cols, int rows) {
    const int cellW = std::max(1, metrics.cellWidth);
    const int cellH = std::max(1, metrics.lineHeight);
    // Clamped, and to cols-1 rather than cols: an IME handed a rectangle
    // outside the widget puts its candidate list off-screen — on Windows,
    // often on whichever monitor owns that coordinate. A composition that runs
    // past the right edge is exactly when that happens, and it is the case a
    // naive implementation never sees, because a short one always fits.
    const int clampedCol = std::clamp(col, 0, std::max(0, cols - 1));
    const int clampedRow = std::clamp(row, 0, std::max(0, rows - 1));
    return CellRect{
        .x = clampedCol * cellW,
        .y = clampedRow * cellH,
        .w = cellW,
        .h = cellH,
    };
}

CellRect preeditRect(const FaceMetrics& metrics, int row, int col, int cells, int cols, int rows) {
    CellRect rect = cursorRect(metrics, row, col, cols, rows);
    rect.w = std::max(1, metrics.cellWidth) * std::max(1, preeditCells(col, cells, cols));
    return rect;
}

}  // namespace krait::render
