#pragma once

#include "render/shaper/shape_pool.h"
#include "render/shaper/shaper.h"

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// DirectWrite interfaces, forward-declared so windows.h and dwrite_3.h stay
// inside fontdb.cpp. COM interfaces are structs under MIDL_INTERFACE, so this is
// the ordinary forward declaration and not a trick.
struct IDWriteFactory2;
struct IDWriteFontCollection;
struct IDWriteFontFallback;

namespace krait::render {

// Font discovery and fallback (T24). DirectWrite answers "which file holds a
// face that can draw this text", FreeType then opens that file — DirectWrite is
// used ONLY for discovery, never for rasterising (ADR-0001).
//
// Thread affinity: Microsoft documents no thread-safety guarantee for
// IDWriteFactory or IDWriteFontFallback — DWRITE_FACTORY_TYPE_SHARED is about
// cache sharing, not locking — so every call here is serialised on m_mutex
// rather than assumed safe. Cheap: resolution happens on font change and on a
// missing-glyph run, not per frame.
class FontDb {
  public:
    FontDb();
    ~FontDb();
    FontDb(const FontDb&) = delete;
    FontDb& operator=(const FontDb&) = delete;

    // False if DirectWrite could not be initialised at all; the caller then has
    // to fall back to a configured font path.
    bool valid() const { return m_fallback != nullptr; }

    // The face for a named family, e.g. "Cascadia Mono". nullopt if the family
    // is not installed, or if it resolves to something FreeType cannot open as
    // a file (see the non-local caveat on resolveFamily()).
    std::optional<FaceSpec> resolve(std::string_view family, bool bold, bool italic,
                                    int pxHeight) const;

    // A face that covers `text`, for a run the primary font could not draw.
    //
    // Preferred families are tried first, by asking each whether it has the
    // run's first codepoint. This exists for Nerd Fonts: their glyphs live in
    // the Private Use Area, where the system fallback has nothing to offer
    // because no shipped font claims those codepoints. Emoji, by contrast, come
    // back from MapCharacters on their own (Segoe UI Emoji), so they need no
    // special case — but they are listed anyway so the intent survives.
    std::optional<FaceSpec> fallbackFor(std::u32string_view text, std::string_view baseFamily,
                                        bool bold, bool italic, int pxHeight) const;

    // Families consulted before the system fallback, in order. Defaults to the
    // common Nerd Font packagings plus Segoe UI Emoji.
    void setPreferredFallbacks(std::vector<std::string> families);

    // The first installed family from `candidates`, for picking a default
    // monospace font without hardcoding one that may be absent.
    std::optional<std::string> firstInstalled(std::span<const std::string_view> candidates) const;

  private:
    struct Resolved {
        std::string path;
        long index = 0;
    };

    // Caller holds m_mutex. nullopt when the font is not backed by a local file
    // (a DWrite in-memory or not-yet-downloaded font). The documented byte path
    // for those is IDWriteFontFileStream + FT_New_Memory_Face, which needs a
    // blob whose lifetime outlives every worker's FT_Face — deliberately not
    // built until something actually needs it.
    std::optional<Resolved> resolveFamily(std::wstring_view family, bool bold, bool italic) const;

    mutable std::mutex m_mutex;
    IDWriteFactory2* m_factory = nullptr;
    IDWriteFontCollection* m_system = nullptr;
    IDWriteFontFallback* m_fallback = nullptr;
    std::vector<std::string> m_preferred;
};

// Shapes `runs` with `primary`, then re-shapes whatever came back with missing
// glyphs using a face the FontDb picked for that run's text (T24's headline
// behaviour). Returns the face id used for each run, parallel to `out`, so the
// renderer knows which face a run's glyph ids belong to — a glyph id is
// meaningless without the face it came from.
//
// A free function rather than a method on either class: it is the one place the
// two meet, and giving ShapePool a FontDb member would drag DirectWrite into
// every test that only wants to shape.
std::vector<std::uint32_t> shapeWithFallback(ShapePool& pool, const FontDb& fonts,
                                             std::span<const Run> runs, std::uint32_t primaryFaceId,
                                             std::string_view primaryFamily, int pxHeight,
                                             bool ligatures, std::vector<ShapedRun>& out);

}  // namespace krait::render
