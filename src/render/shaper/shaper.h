#pragma once

#include "render/atlas/glyph_atlas.h"  // GlyphBitmap
#include "render/shaper/shaped_run.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// FreeType and HarfBuzz handles, forward-declared so this header stays out of
// their include order (ft2build.h has to come first, and only shaper.cpp cares).
struct FT_LibraryRec_;
struct FT_FaceRec_;
struct hb_font_t;
struct hb_buffer_t;

namespace krait::render {

// Where a face comes from. Immutable once registered with a ShapePool.
//
// pxHeight is part of the face's IDENTITY, not a parameter of shaping: hb-ft
// snapshots the FT_Face's size when the hb_font_t is created, so a size change
// means a different face id rather than a mutation (which would otherwise need
// hb_ft_font_changed and a cache flush).
struct FaceSpec {
    std::string path;   // font file, opened independently by each worker
    long index = 0;     // face index inside a TTC (IDWriteFontFace::GetIndex)
    int pxHeight = 16;  // FT_Set_Pixel_Sizes
};

// Pixel metrics the renderer needs to lay out the grid.
struct FaceMetrics {
    // The cell advance, from the hinted advance of 'M'. NOT
    // face->size->metrics.max_advance: that is the maximum over EVERY glyph in
    // the face, so one CJK ideograph, box-drawing glyph or powerline arrow in
    // the file doubles it. The M0 spike used max_advance and got away with it
    // only because its font had none of those.
    int cellWidth = 0;
    int ascent = 0;   // px above the baseline (26.6 rounded up by FreeType)
    int descent = 0;  // px below, positive
    int lineHeight = 0;
};

// One thread's shaping engine. NOT thread-safe, deliberately: FreeType
// documents that "an FT_Face object can only be safely used from one thread at
// a time" and that creating faces on one FT_Library must also be serialised, and
// hb-ft adds "FreeType is not thread-safe, therefore these functions are not
// thread-safe either". So each worker owns a whole private stack — its own
// FT_Library, its own FT_Face per registered face, its own hb_font_t — and
// nothing is shared but the immutable FaceSpec list.
class Shaper {
  public:
    Shaper();
    ~Shaper();
    Shaper(const Shaper&) = delete;
    Shaper& operator=(const Shaper&) = delete;

    // Opens `spec` as `faceId`. False if FreeType cannot load the file.
    bool loadFace(std::uint32_t faceId, const FaceSpec& spec);

    bool hasFace(std::uint32_t faceId) const;

    std::optional<FaceMetrics> metrics(std::uint32_t faceId) const;

    // Shapes one run. An unloaded faceId or an empty run yields no glyphs
    // rather than an error: a frame missing a face must still draw.
    //
    // `ligatures == false` disables liga, calt and dlig. calt matters most —
    // it is what actually produces the arrows in Fira Code and friends, and it
    // is on by default, so a toggle that only touched liga would look broken.
    ShapedRun shape(const Run& run, std::uint32_t faceId, bool ligatures);

    // Rasterises one glyph into `out` for the atlas. False if the face is not
    // loaded or FreeType cannot render the glyph.
    //
    // Hinting is LIGHT, which hints vertically only and leaves horizontal
    // metrics alone. That is the pairing this design needs, not a preference:
    // hb-ft reports UNHINTED advances (it loads with FT_LOAD_NO_HINTING), so a
    // fully hinted raster would disagree with the positions the shaper already
    // produced.
    bool rasterize(std::uint32_t faceId, std::uint32_t glyphId, GlyphBitmap& out);

  private:
    struct FaceSlot {
        std::uint32_t id = 0;
        FT_FaceRec_* face = nullptr;
        hb_font_t* font = nullptr;
        FaceMetrics metrics;
    };

    const FaceSlot* find(std::uint32_t faceId) const;

    FT_LibraryRec_* m_ft = nullptr;
    hb_buffer_t* m_buf = nullptr;
    std::vector<FaceSlot> m_faces;
};

}  // namespace krait::render
