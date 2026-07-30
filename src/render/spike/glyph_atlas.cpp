#include "glyph_atlas.h"

#include <QFile>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace krait::render::spike {

GlyphAtlas buildAsciiAtlas(const QStringList& fontPaths, int pixelHeight) {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        return {};
    }
    FT_Face face = nullptr;
    for (const QString& path : fontPaths) {
        if (QFile::exists(path) &&
            FT_New_Face(library, path.toLocal8Bit().constData(), 0, &face) == 0) {
            break;
        }
        face = nullptr;
    }
    if (face == nullptr) {
        FT_Done_FreeType(library);
        return {};
    }
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelHeight));

    GlyphAtlas atlas;
    atlas.cellWidth = static_cast<int>(face->size->metrics.max_advance >> 6);
    atlas.baseline = static_cast<int>(face->size->metrics.ascender >> 6);
    atlas.cellHeight = atlas.baseline - static_cast<int>(face->size->metrics.descender >> 6);

    constexpr int kFirst = 0x20;
    constexpr int kCount = 95;  // 0x20..0x7E
    atlas.image = QImage(atlas.cellWidth * kCount, atlas.cellHeight, QImage::Format_Grayscale8);
    atlas.image.fill(0);

    for (int i = 0; i < kCount; ++i) {
        if (FT_Load_Char(face, static_cast<FT_ULong>(kFirst + i), FT_LOAD_RENDER) != 0) {
            continue;
        }
        const FT_Bitmap& bmp = face->glyph->bitmap;
        if (bmp.pixel_mode != FT_PIXEL_MODE_GRAY || bmp.pitch < 0) {
            continue;  // mono strikes / negative pitch: skip, don't OOB
        }
        const int x0 = i * atlas.cellWidth + face->glyph->bitmap_left;
        const int y0 = atlas.baseline - face->glyph->bitmap_top;
        for (unsigned row = 0; row < bmp.rows; ++row) {
            const int y = y0 + static_cast<int>(row);
            if (y < 0 || y >= atlas.cellHeight) {
                continue;
            }
            uchar* dst = atlas.image.scanLine(y);
            for (unsigned col = 0; col < bmp.width; ++col) {
                const int x = x0 + static_cast<int>(col);
                if (x < i * atlas.cellWidth || x >= (i + 1) * atlas.cellWidth) {
                    continue;  // clip into the glyph's own cell
                }
                dst[x] = bmp.buffer[row * static_cast<unsigned>(bmp.pitch) + col];
            }
        }
    }

    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return atlas;
}

}  // namespace krait::render::spike
