#include "render/shaper/shaper.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace krait::render {
namespace {

// 26.6 fixed point to whole pixels, rounded to nearest.
int toPx(FT_Pos fixed) {
    return static_cast<int>((fixed + 32) >> 6);
}

// The cell advance. FreeType's max_advance is the maximum over EVERY glyph in
// the face, so a single CJK ideograph or powerline arrow in the file inflates
// it; the advance of a real ASCII glyph is the monospace cell. Tried in order,
// because a Thai- or CJK-only fallback face may have no 'M' at all.
int cellAdvance(FT_Face face) {
    for (const FT_ULong probe : {FT_ULong{'M'}, FT_ULong{'0'}, FT_ULong{'x'}}) {
        const FT_UInt gid = FT_Get_Char_Index(face, probe);
        if (gid != 0 && FT_Load_Glyph(face, gid, FT_LOAD_DEFAULT) == 0) {
            const int advance = toPx(face->glyph->advance.x);
            if (advance > 0) {
                return advance;
            }
        }
    }
    return toPx(face->size->metrics.max_advance);
}

}  // namespace

Shaper::Shaper() {
    if (FT_Init_FreeType(&m_ft) != 0) {
        m_ft = nullptr;
        return;
    }
    m_buf = hb_buffer_create();
    // MONOTONE_CHARACTERS (level 1) instead of the default level 0. Level 0
    // merges a combining mark's cluster into the preceding base's, which is
    // right for a caller that hands HarfBuzz raw text and wrong for one that
    // already segmented it. Ours is one cluster per cell today, so the two
    // agree — set it explicitly so they still agree if a run ever carries more.
    hb_buffer_set_cluster_level(m_buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
}

Shaper::~Shaper() {
    // hb fonts first: each holds a reference on its FT_Face taken by
    // hb_ft_font_create_referenced, and dropping that before our own
    // FT_Done_Face keeps the face alive exactly until nothing points at it.
    for (FaceSlot& slot : m_faces) {
        if (slot.font != nullptr) {
            hb_font_destroy(slot.font);
        }
        if (slot.face != nullptr) {
            FT_Done_Face(slot.face);
        }
    }
    if (m_buf != nullptr) {
        hb_buffer_destroy(m_buf);
    }
    if (m_ft != nullptr) {
        FT_Done_FreeType(m_ft);
    }
}

bool Shaper::loadFace(std::uint32_t faceId, const FaceSpec& spec) {
    if (m_ft == nullptr) {
        return false;
    }

    FT_Face face = nullptr;
    if (FT_New_Face(m_ft, spec.path.c_str(), spec.index, &face) != 0) {
        return false;
    }
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(spec.pxHeight)) != 0) {
        FT_Done_Face(face);
        return false;
    }
    // The size MUST be set before this call: hb-ft snapshots the FT_Face's
    // scale here, and the "_referenced" spelling is the one that takes its own
    // reference on the face ("use this version unless you know you have good
    // reasons not to"). Do NOT follow it with hb_ft_font_set_funcs — that
    // builds hb its OWN FT_Face internally and would discard this one.
    hb_font_t* font = hb_ft_font_create_referenced(face);
    if (font == nullptr) {
        FT_Done_Face(face);
        return false;
    }

    FaceSlot slot;
    slot.id = faceId;
    slot.face = face;
    slot.font = font;
    slot.metrics.ascent = toPx(face->size->metrics.ascender);
    slot.metrics.descent = -toPx(face->size->metrics.descender);  // reported negative
    slot.metrics.lineHeight = toPx(face->size->metrics.height);
    slot.metrics.cellWidth = cellAdvance(face);

    const auto it =
        std::ranges::find_if(m_faces, [faceId](const FaceSlot& s) { return s.id == faceId; });
    if (it != m_faces.end()) {
        hb_font_destroy(it->font);
        FT_Done_Face(it->face);
        *it = slot;
    } else {
        m_faces.push_back(slot);
    }
    return true;
}

bool Shaper::hasFace(std::uint32_t faceId) const {
    return find(faceId) != nullptr;
}

std::optional<FaceMetrics> Shaper::metrics(std::uint32_t faceId) const {
    const FaceSlot* slot = find(faceId);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return slot->metrics;
}

const Shaper::FaceSlot* Shaper::find(std::uint32_t faceId) const {
    const auto it =
        std::ranges::find_if(m_faces, [faceId](const FaceSlot& s) { return s.id == faceId; });
    return it == m_faces.end() ? nullptr : &*it;
}

ShapedRun Shaper::shape(const Run& run, std::uint32_t faceId, bool ligatures) {
    ShapedRun out;
    out.faceId = faceId;
    const FaceSlot* slot = find(faceId);
    if (slot == nullptr || run.text.empty() || run.clusters.empty()) {
        return out;
    }

    // clear_contents, NOT reset: reset would also drop the cluster level set in
    // the constructor and silently re-merge mark clusters.
    hb_buffer_clear_contents(m_buf);
    const auto len = static_cast<int>(run.text.size());
    hb_buffer_add_utf32(m_buf, reinterpret_cast<const std::uint32_t*>(run.text.data()), len, 0,
                        len);
    hb_buffer_set_direction(m_buf, run.rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_script(m_buf, static_cast<hb_script_t>(run.script));
    // Language deliberately left unset. hb_buffer_guess_segment_properties()
    // would fill it from the process locale, making shaping depend on the
    // machine and the tests non-reproducible; nothing we ship needs a
    // language-specific `locl` substitution.

    std::array<hb_feature_t, 3> features{};
    unsigned int featureCount = 0;
    if (!ligatures) {
        features = {
            hb_feature_t{HB_TAG('l', 'i', 'g', 'a'), 0, HB_FEATURE_GLOBAL_START,
                         HB_FEATURE_GLOBAL_END},
            hb_feature_t{HB_TAG('c', 'a', 'l', 't'), 0, HB_FEATURE_GLOBAL_START,
                         HB_FEATURE_GLOBAL_END},
            hb_feature_t{HB_TAG('d', 'l', 'i', 'g'), 0, HB_FEATURE_GLOBAL_START,
                         HB_FEATURE_GLOBAL_END},
        };
        featureCount = static_cast<unsigned int>(features.size());
    }
    hb_shape(slot->font, m_buf, features.data(), featureCount);

    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(m_buf, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(m_buf, nullptr);
    if (infos == nullptr || positions == nullptr) {
        return out;
    }

    // HarfBuzz reports the cluster value we fed in, i.e. an index into the run's
    // TEXT. Map it back to the run's cluster (and so to a column) by binary
    // search rather than a forward cursor: an RTL run comes back in visual
    // order, so the cluster values are not monotone in output order.
    const auto clusterOf = [&run](std::uint32_t cpIndex) {
        const auto it = std::ranges::upper_bound(run.clusters, cpIndex, {}, &ClusterRef::offset);
        const auto index = (it - run.clusters.begin()) - 1;
        return static_cast<std::uint32_t>(index < 0 ? 0 : index);
    };

    out.glyphs.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        // After shaping, info.codepoint IS a glyph index (HarfBuzz reuses the
        // field). Glyph 0 is .notdef, which is a reliable "this face lacks
        // coverage" signal only because we never call
        // hb_buffer_set_not_found_glyph to change it — T24's fallback chain
        // depends on that, so do not start setting it.
        out.glyphs.push_back(ShapedGlyph{
            .glyphId = infos[i].codepoint,
            .xAdvance = positions[i].x_advance,
            .yAdvance = positions[i].y_advance,
            .xOffset = positions[i].x_offset,
            .yOffset = positions[i].y_offset,
            .cluster = clusterOf(infos[i].cluster),
        });
        if (infos[i].codepoint == 0) {
            out.missingGlyphs = true;
        }
    }
    return out;
}

}  // namespace krait::render
