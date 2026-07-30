#pragma once

#include "core/grid/damage.h"
#include "core/grid/line.h"
#include "render/atlas/glyph_atlas.h"
#include "render/shaper/shaped_run.h"
#include "render/shaper/shaper.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace krait::render {

// A solid rectangle in pixels: cell backgrounds, selection, cursor, underlines,
// strikethrough. One pipeline draws all of them because they differ only in
// colour and extent.
struct SolidInstance {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1;
};

// One textured glyph quad. `u*`/`v*` are normalised atlas coordinates.
struct GlyphInstance {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    float u0 = 0;
    float v0 = 0;
    float u1 = 0;
    float v1 = 0;
    float r = 1;
    float g = 1;
    float b = 1;
    float a = 1;
};

enum class CursorStyle : std::uint8_t { Block, HollowBlock, Underline, Bar };

// Theme colours as sRGB 0xRRGGBB. rules/render.md: theme colours are data, never
// literals in render code — this struct is that data, and T31 fills it from the
// settings registry.
struct Theme {
    std::array<std::uint32_t, 16> ansi{0x45475A, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7,
                                       0x94E2D5, 0xBAC2DE, 0x585B70, 0xF38BA8, 0xA6E3A1, 0xF9E2AF,
                                       0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xA6ADC8};
    std::uint32_t fg = 0xCDD6F4;
    std::uint32_t bg = 0x1E1E2E;
    std::uint32_t cursor = 0xF5E0DC;
    std::uint32_t cursorText = 0x1E1E2E;
    std::uint32_t selectionBg = 0x45475A;
};

// An inclusive text selection in viewport coordinates. Rows are viewport rows,
// not scrollback indices.
struct Selection {
    bool active = false;
    int anchorRow = 0;
    int anchorCol = 0;
    int cursorRow = 0;
    int cursorCol = 0;
};

struct CursorState {
    bool visible = true;
    bool focused = true;  // an unfocused terminal shows a hollow block
    int row = 0;
    int col = 0;
    CursorStyle style = CursorStyle::Block;
};

// Everything one frame needs that is not the grid itself.
struct FrameParams {
    Selection selection;
    CursorState cursor;
    int cols = 0;
};

// Turns viewport rows plus shaped runs into the two instance arrays the GPU
// draws (T25). Deliberately Qt-free and GPU-free: cursor styles, selection
// rectangles and the damage-driven rebuild are all assertable on the CPU, which
// a golden image can only show indirectly.
//
// Damage discipline (rules/render.md: "a full-frame redraw outside resize or
// theme-change is a defect"): instances are cached PER ROW and only damaged rows
// are rebuilt. rowsRebuilt() reports how many actually were, so a test can prove
// a clean row was skipped rather than merely assume it.
class FrameBuilder {
  public:
    FrameBuilder(FaceMetrics metrics, Theme theme);

    // The shaped runs for one row, and which face each came from — the shape of
    // what shapeWithFallback returns.
    struct RowRuns {
        std::span<const Run> runs;
        std::span<const ShapedRun> shaped;
        std::span<const std::uint32_t> faces;
    };

    // Rebuilds the damaged rows. `rowRuns(row)` is called only for rows being
    // rebuilt, so the caller can skip shaping clean rows entirely.
    //
    // Cursor and selection are appended every frame regardless: they move
    // without the grid changing, and both are cheap.
    void build(std::span<const core::vt::Line> viewport, const core::vt::DamageList& damage,
               const FrameParams& params, const RasterFn& raster, GlyphAtlas& atlas,
               const std::function<RowRuns(int row)>& rowRuns);

    std::span<const SolidInstance> solids() const { return m_solids; }

    std::span<const GlyphInstance> glyphs() const { return m_glyphs; }

    int rowsRebuilt() const { return m_rowsRebuilt; }

    // Forces every row to rebuild on the next build() — for a theme change, a
    // font change or a resize, the three cases where a full redraw is correct.
    void invalidate();

    const FaceMetrics& metrics() const { return m_metrics; }

    void setTheme(Theme theme);

    const Theme& theme() const { return m_theme; }

    int cellHeight() const { return m_metrics.lineHeight; }

    int pixelWidth(int cols) const { return cols * m_metrics.cellWidth; }

    int pixelHeight(int rows) const { return rows * cellHeight(); }

  private:
    struct RowCache {
        std::vector<SolidInstance> solids;
        std::vector<GlyphInstance> glyphs;
        bool valid = false;
    };

    void buildRow(int row, const core::vt::Line& line, const RowRuns& runs, const RasterFn& raster,
                  GlyphAtlas& atlas, RowCache& out) const;

    void appendSelection(const FrameParams& params, int rowCount);
    void appendCursor(std::span<const core::vt::Line> viewport, const FrameParams& params);

    FaceMetrics m_metrics;
    Theme m_theme;
    std::vector<RowCache> m_rows;
    std::vector<SolidInstance> m_solids;
    std::vector<GlyphInstance> m_glyphs;
    int m_rowsRebuilt = 0;
    bool m_invalidated = true;
};

// sRGB 0xRRGGBB to the three floats an instance carries.
void unpackColor(std::uint32_t rgb, float& r, float& g, float& b);

// Resolves a cell's foreground and background to concrete colours, applying
// reverse video, bold-brightening, dim and the invisible flag.
void resolveColors(const core::vt::Attr& attr, const Theme& theme, std::uint32_t& fg,
                   std::uint32_t& bg);

// The xterm 256-colour palette entry for an index, using `theme` for 0-15.
std::uint32_t paletteColor(std::uint8_t index, const Theme& theme);

// Whether a viewport cell falls inside a selection. Exposed because the
// start/end ordering is easy to get subtly wrong and deserves its own test.
bool selectionContains(const Selection& selection, int row, int col);

}  // namespace krait::render
