#pragma once

#include "core/grid/cluster_pool.h"
#include "core/grid/damage.h"
#include "core/grid/images.h"
#include "core/grid/line.h"
#include "render/atlas/glyph_atlas.h"
#include "render/shaper/shaped_run.h"
#include "render/shaper/shaper.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// One image quad (T84). Deliberately the SAME layout as GlyphInstance —
// position, uv, colour — so images reuse glyph.vert and the glyph pipeline's
// vertex input layout unchanged. Only the fragment shader differs: an RGBA
// sample instead of the atlas's R8 coverage. `a` carries the placement's
// opacity with rgb left at 1.
using ImageInstance = GlyphInstance;

// A run of image quads that share one texture. A terminal shows a handful of
// images at once, so a run list beside one instance buffer is cheaper — and far
// less code — than a vertex buffer per texture.
struct ImageBatch {
    std::uint32_t imageId = 0;
    std::uint32_t first = 0;  // index into FrameBuilder::images()
    std::uint32_t count = 0;
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
    // T68's trigger highlight. A different colour from the selection on
    // purpose: the two can overlap, and a highlight that looks like a selection
    // would have the user pressing Ctrl+Shift+C over text they never selected.
    std::uint32_t highlightBg = 0xF9E2AF;
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

// A trigger match to paint (T68), in viewport coordinates. Half-open columns.
//
// Supplied per frame rather than cached with the row, and derived from the
// VISIBLE text rather than from where a match landed when it arrived: a
// coordinate recorded at feed time is invalidated by the next reflow, which is
// the scrollback landmine CLAUDE.md names. Re-deriving it from what is on
// screen cannot go stale.
struct HighlightSpan {
    int row = 0;
    int beginCol = 0;
    int endCol = 0;
};

// Everything one frame needs that is not the grid itself.
struct FrameParams {
    Selection selection;
    CursorState cursor;
    int cols = 0;
    std::span<const HighlightSpan> highlights;

    // --- Graphics placements (T84) ---
    //
    // `rowStable` is PARALLEL to the viewport: entry r is the stable index of
    // the logical line viewport row r belongs to. The caller derives it by
    // walking the rows it is about to draw, starting from
    // Grid::viewportTopStable() — from the rows themselves, so it cannot
    // disagree with them. A placement is drawn on the first viewport row whose
    // entry equals its anchor.
    std::span<const core::vt::Placement> placements;
    std::span<const std::uint64_t> rowStable;
    // Where the pixels live, for the source size the UVs are normalised
    // against. Null means "draw no images", which is the pre-graphics path and
    // every test that does not care.
    const core::vt::ImageStore* images = nullptr;

    // The atlas OSC 66's scaled glyphs go in (T84). It has to be a second
    // atlas: a slot in the main one is sized for a two-cell glyph on one line,
    // and GlyphAtlas REJECTS anything larger rather than clipping it — so
    // shaping sized text at its real pixel height and then asking the main
    // atlas for it would draw nothing at all, which is worse than the 1x it
    // replaced. Null means no sized text is expected this frame.
    GlyphAtlas* sizedAtlas = nullptr;
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

    // OSC 66's scaled glyphs, which sample the SIZED atlas rather than the main
    // one and so need their own draw (T84).
    std::span<const GlyphInstance> sizedGlyphs() const { return m_sizedGlyphs; }

    // Image quads, ordered: the zIndex < 0 ones first (they draw UNDER the
    // text, which is what a watermark is), then the rest. `imageBatches()`
    // slices this into per-texture runs, and the first `belowBatchCount()` of
    // those are the under-text ones.
    std::span<const ImageInstance> images() const { return m_images; }

    std::span<const ImageBatch> imageBatches() const { return m_imageBatches; }

    std::uint32_t belowBatchCount() const { return m_belowBatches; }

    int rowsRebuilt() const { return m_rowsRebuilt; }

    // Whether build() will rebuild this row, given the same damage. Public so a
    // caller can split and shape every damaged row in ONE batch before calling
    // build(): shaping per row means one blocking round trip to the worker pool
    // per row, which is 63 waits a frame on a full screen. Must be called
    // BEFORE build(), which consumes the invalidation flag.
    bool rowNeedsRebuild(int row, const core::vt::DamageList& damage) const;

    // Forces every row to rebuild on the next build() — for a theme change, a
    // font change or a resize, the three cases where a full redraw is correct.
    void invalidate();

    const FaceMetrics& metrics() const { return m_metrics; }

    // Appends one already-shaped run's glyphs at a fixed colour (plan T29).
    //
    // The IME composition needs this: a preedit is NOT grid content — it
    // belongs to the IME until it commits — so it has no cells to read colours
    // from, and build() cannot draw it. Sharing the placement code is the
    // point: glyph position is cluster column + shaper offset + font bearing,
    // and a second copy of that arithmetic is how a composition ends up
    // drawing one pixel off from the text it commits to.
    void appendShapedRun(const Run& run, const ShapedRun& shaped, std::uint32_t faceId,
                         std::uint32_t fg, const RasterFn& raster, GlyphAtlas& atlas,
                         std::vector<GlyphInstance>& out) const;

    void setTheme(Theme theme);

    const Theme& theme() const { return m_theme; }

    int cellHeight() const { return m_metrics.lineHeight; }

    int pixelWidth(int cols) const { return cols * m_metrics.cellWidth; }

    int pixelHeight(int rows) const { return rows * cellHeight(); }

  private:
    struct RowCache {
        std::vector<SolidInstance> solids;
        std::vector<GlyphInstance> glyphs;
        std::vector<GlyphInstance> sizedGlyphs;
        bool valid = false;
    };

    void buildRow(int row, const core::vt::Line& line, const RowRuns& runs, const RasterFn& raster,
                  GlyphAtlas& atlas, GlyphAtlas* sizedAtlas, RowCache& out) const;

    void appendHighlights(const FrameParams& params, int rowCount);
    void appendSelection(const FrameParams& params, int rowCount);
    void appendCursor(std::span<const core::vt::Line> viewport, const FrameParams& params);
    void appendImages(const FrameParams& params);

    FaceMetrics m_metrics;
    Theme m_theme;
    std::vector<RowCache> m_rows;
    std::vector<SolidInstance> m_solids;
    std::vector<GlyphInstance> m_glyphs;
    std::vector<GlyphInstance> m_sizedGlyphs;

    // Images are NOT part of the row cache. One placement spans several rows
    // and moves with the viewport rather than with any single row's content, so
    // caching it per row would need an invalidation rule of its own — the same
    // reason the cursor and the selection are rebuilt every frame. Rebuilding
    // them also means a scrolled viewport cannot smear a stale quad, because
    // there is no stale quad to keep.
    std::vector<ImageInstance> m_images;
    std::vector<ImageBatch> m_imageBatches;
    std::uint32_t m_belowBatches = 0;
    // Reused across frames so a frame with placements does not allocate. Maps a
    // stable line index to the FIRST viewport row that shows it.
    std::unordered_map<std::uint64_t, int> m_rowOfStable;
    // The distinct image ids this frame draws, so the count can be bounded to
    // what GpuResources will actually keep textures for.
    std::unordered_set<std::uint32_t> m_distinctImages;
    // Placement indices, sorted for draw order. Indices rather than copies:
    // sorting Placements would copy nine ints each for nothing.
    std::vector<std::uint32_t> m_sortedPlacements;

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

// The selected text as UTF-8, for the clipboard (T27).
//
// Lives beside selectionContains because it has to agree with it cell for cell;
// two independent notions of "selected" is how a terminal ends up copying text
// the user never saw highlighted.
//
// Trailing blanks are trimmed per row and rows are joined with "\n" — EXCEPT
// where the next row carries wrappedFromPrev. A wrapped line is ONE logical
// line, and breaking it is what turns a copied command into two broken ones
// when it is pasted back.
std::string selectionText(std::span<const core::vt::Line> viewport, const Selection& selection,
                          const core::vt::ClusterPool& clusters);

}  // namespace krait::render
