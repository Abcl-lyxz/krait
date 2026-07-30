#include "render/shaper/fontdb.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dwrite_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <utility>

namespace krait::render {
namespace {

using Microsoft::WRL::ComPtr;

// UTF-32 to UTF-16. DirectWrite counts everything in UTF-16 code units, so this
// conversion is also what makes MapCharacters' textLength meaningful.
std::wstring toUtf16(std::u32string_view text) {
    std::wstring out;
    out.reserve(text.size() + 4);
    for (const char32_t cp : text) {
        // Lone surrogates and out-of-range values cannot come from our UTF-8
        // decoder, but the cell content is remote-controlled (rules/net.md) and
        // handing DirectWrite an invalid unit is not worth the risk.
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            continue;
        }
        if (cp < 0x10000) {
            out.push_back(static_cast<wchar_t>(cp));
            continue;
        }
        const char32_t v = cp - 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
    }
    return out;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int len =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

// The minimum IDWriteTextAnalysisSource that MapCharacters accepts: one block of
// text, one locale, no number substitution. There is no documented overload that
// takes a raw string, so this stub is mandatory rather than a convenience.
//
// Lifetime: always a stack local passed straight into MapCharacters, which does
// not retain it. AddRef/Release therefore only have to be well-behaved, not to
// own anything — the object must NOT delete itself.
class SingleRunSource final : public IDWriteTextAnalysisSource {
  public:
    explicit SingleRunSource(const std::wstring& text) : m_text(text) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IDWriteTextAnalysisSource) || riid == __uuidof(IUnknown)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ULONG STDMETHODCALLTYPE Release() override { return --m_refs; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, WCHAR const** text,
                                                UINT32* length) override {
        if (position >= m_text.size()) {
            *text = nullptr;  // documented end-of-text answer
            *length = 0;
            return S_OK;
        }
        *text = m_text.c_str() + position;
        *length = static_cast<UINT32>(m_text.size() - position);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, WCHAR const** text,
                                                    UINT32* length) override {
        if (position == 0 || position > m_text.size()) {
            *text = nullptr;
            *length = 0;
            return S_OK;
        }
        *text = m_text.c_str();
        *length = position;
        return S_OK;
    }

    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* length,
                                            WCHAR const** localeName) override {
        *length = position < m_text.size() ? static_cast<UINT32>(m_text.size() - position) : 0;
        *localeName = nullptr;  // no locale preference; DirectWrite uses its default
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(
        UINT32 position, UINT32* length, IDWriteNumberSubstitution** substitution) override {
        *length = position < m_text.size() ? static_cast<UINT32>(m_text.size() - position) : 0;
        // Documented: "rather than return E_NOTIMPL, an application should stub
        // the method and return a constant/null and S_OK".
        *substitution = nullptr;
        return S_OK;
    }

  private:
    const std::wstring& m_text;
    ULONG m_refs = 1;
};

constexpr DWRITE_FONT_WEIGHT weightFor(bool bold) {
    return bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_REGULAR;
}

constexpr DWRITE_FONT_STYLE styleFor(bool italic) {
    return italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
}

// The font file behind a resolved IDWriteFont, plus its index inside a TTC.
struct FileRef {
    std::string path;
    long index = 0;
};

std::optional<FileRef> fileRefOf(IDWriteFont* font) {
    ComPtr<IDWriteFontFace> face;
    if (font == nullptr || FAILED(font->CreateFontFace(&face))) {
        return std::nullopt;
    }

    // Documented two-call pattern: null buffer asks for the count.
    UINT32 fileCount = 0;
    if (FAILED(face->GetFiles(&fileCount, nullptr)) || fileCount == 0) {
        return std::nullopt;
    }
    std::vector<ComPtr<IDWriteFontFile>> files(fileCount);
    if (FAILED(face->GetFiles(&fileCount, files[0].GetAddressOf()))) {
        return std::nullopt;
    }

    const void* key = nullptr;
    UINT32 keySize = 0;
    if (FAILED(files[0]->GetReferenceKey(&key, &keySize))) {
        return std::nullopt;
    }
    ComPtr<IDWriteFontFileLoader> loader;
    if (FAILED(files[0]->GetLoader(&loader))) {
        return std::nullopt;
    }
    ComPtr<IDWriteLocalFontFileLoader> local;
    if (FAILED(loader.As(&local))) {
        // Not a local file — a DWrite in-memory or not-yet-downloaded font. The
        // documented byte path is CreateStreamFromKey + ReadFileFragment into
        // FT_New_Memory_Face, which needs a blob outliving every worker's
        // FT_Face. ponytail: unbuilt until a real font needs it; system fonts,
        // which is all T24 resolves, are always local files.
        return std::nullopt;
    }

    UINT32 pathLength = 0;  // excludes the NUL
    if (FAILED(local->GetFilePathLengthFromKey(key, keySize, &pathLength))) {
        return std::nullopt;
    }
    std::wstring path(static_cast<std::size_t>(pathLength) + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, keySize, path.data(), pathLength + 1))) {
        return std::nullopt;
    }
    path.resize(pathLength);

    // ponytail: FreeType's FT_New_Face takes a char* the CRT interprets in the
    // ANSI codepage, so a font under a path with non-ASCII characters will fail
    // to open and surface as a failed registerFace. Every system font lives
    // under an ASCII path, so this only bites user-supplied fonts (T31);
    // the fix then is FT_New_Memory_Face over bytes we read with the wide API.
    return FileRef{.path = narrow(path), .index = static_cast<long>(face->GetIndex())};
}

}  // namespace

FontDb::FontDb()
    : m_preferred{// Symbols-only Nerd Font first: it exists precisely to be a fallback,
                  // so it wins over a full Nerd Font whose Latin would never be used
                  // here anyway. Emoji is listed for intent — MapCharacters already
                  // answers Segoe UI Emoji on its own.
                  "Symbols Nerd Font Mono",
                  "Symbols Nerd Font",
                  "CaskaydiaCove Nerd Font Mono",
                  "JetBrainsMono Nerd Font Mono",
                  "FiraCode Nerd Font Mono",
                  "Hack Nerd Font Mono",
                  "Segoe UI Emoji"} {
    // DWriteCreateFactory's out-param is IUnknown**, so IID_PPV_ARGS does not
    // type-check here; the reinterpret_cast is the documented form.
    ComPtr<IDWriteFactory2> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                   reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
        return;
    }

    ComPtr<IDWriteFontFallback> fallback;
    if (FAILED(factory->GetSystemFontFallback(&fallback))) {
        return;
    }
    ComPtr<IDWriteFontCollection> system;
    if (FAILED(factory->GetSystemFontCollection(&system, FALSE))) {
        return;
    }

    // Ownership moves to the raw members so the header needs no WRL include.
    m_factory = factory.Detach();
    m_system = system.Detach();
    m_fallback = fallback.Detach();
}

FontDb::~FontDb() {
    if (m_fallback != nullptr) {
        m_fallback->Release();
    }
    if (m_system != nullptr) {
        m_system->Release();
    }
    if (m_factory != nullptr) {
        m_factory->Release();
    }
}

void FontDb::setPreferredFallbacks(std::vector<std::string> families) {
    const std::lock_guard lock(m_mutex);
    m_preferred = std::move(families);
}

std::optional<FontDb::Resolved> FontDb::resolveFamily(std::wstring_view family, bool bold,
                                                      bool italic) const {
    if (m_system == nullptr || family.empty()) {
        return std::nullopt;
    }
    const std::wstring name(family);

    UINT32 index = 0;
    BOOL exists = FALSE;
    // Returns S_OK with exists == FALSE for an absent family, so the HRESULT
    // alone is not the answer.
    if (FAILED(m_system->FindFamilyName(name.c_str(), &index, &exists)) || exists == FALSE) {
        return std::nullopt;
    }
    ComPtr<IDWriteFontFamily> fonts;
    if (FAILED(m_system->GetFontFamily(index, &fonts))) {
        return std::nullopt;
    }
    ComPtr<IDWriteFont> font;
    // (weight, STRETCH, style). MapCharacters below takes (weight, style,
    // stretch) — the opposite. All three are enums, so swapping them compiles
    // silently and only shows up as the wrong face.
    if (FAILED(fonts->GetFirstMatchingFont(weightFor(bold), DWRITE_FONT_STRETCH_NORMAL,
                                           styleFor(italic), &font))) {
        return std::nullopt;
    }
    const auto ref = fileRefOf(font.Get());
    if (!ref.has_value()) {
        return std::nullopt;
    }
    return Resolved{.path = ref->path, .index = ref->index};
}

std::optional<FaceSpec> FontDb::resolve(std::string_view family, bool bold, bool italic,
                                        int pxHeight) const {
    const std::lock_guard lock(m_mutex);
    const auto resolved = resolveFamily(widen(family), bold, italic);
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    return FaceSpec{.path = resolved->path, .index = resolved->index, .pxHeight = pxHeight};
}

std::optional<std::string>
FontDb::firstInstalled(std::span<const std::string_view> candidates) const {
    const std::lock_guard lock(m_mutex);
    if (m_system == nullptr) {
        return std::nullopt;
    }
    for (const std::string_view candidate : candidates) {
        const std::wstring name = widen(candidate);
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(m_system->FindFamilyName(name.c_str(), &index, &exists)) && exists != FALSE) {
            return std::string(candidate);
        }
    }
    return std::nullopt;
}

std::optional<FaceSpec> FontDb::fallbackFor(std::u32string_view text, std::string_view baseFamily,
                                            bool bold, bool italic, int pxHeight) const {
    if (text.empty()) {
        return std::nullopt;
    }
    const std::lock_guard lock(m_mutex);
    if (m_fallback == nullptr) {
        return std::nullopt;
    }

    // Preferred families first. HasCharacter is a coverage question, so it
    // answers for the Private Use Area, where the system fallback cannot: no
    // shipped Windows font claims those codepoints, which is exactly why a Nerd
    // Font glyph would otherwise come back unmapped.
    const char32_t probe = text.front();
    for (const std::string& family : m_preferred) {
        const std::wstring name = widen(family);
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(m_system->FindFamilyName(name.c_str(), &index, &exists)) || exists == FALSE) {
            continue;
        }
        ComPtr<IDWriteFontFamily> fonts;
        if (FAILED(m_system->GetFontFamily(index, &fonts))) {
            continue;
        }
        ComPtr<IDWriteFont> font;
        if (FAILED(fonts->GetFirstMatchingFont(weightFor(bold), DWRITE_FONT_STRETCH_NORMAL,
                                               styleFor(italic), &font))) {
            continue;
        }
        BOOL hasIt = FALSE;
        // HasCharacter takes a UCS-4 code point, not a UTF-16 unit.
        if (FAILED(font->HasCharacter(static_cast<UINT32>(probe), &hasIt)) || hasIt == FALSE) {
            continue;
        }
        if (const auto ref = fileRefOf(font.Get())) {
            return FaceSpec{.path = ref->path, .index = ref->index, .pxHeight = pxHeight};
        }
    }

    // Then the system fallback.
    const std::wstring utf16 = toUtf16(text);
    if (utf16.empty()) {
        return std::nullopt;
    }
    SingleRunSource source(utf16);
    const std::wstring base = widen(baseFamily);

    UINT32 mappedLength = 0;
    FLOAT scale = 1.0F;
    ComPtr<IDWriteFont> mapped;
    // (weight, style, stretch) here — see the note in resolveFamily().
    const HRESULT hr = m_fallback->MapCharacters(
        &source, 0, static_cast<UINT32>(utf16.size()), m_system,
        base.empty() ? nullptr : base.c_str(), weightFor(bold), styleFor(italic),
        DWRITE_FONT_STRETCH_NORMAL, &mappedLength, &mapped, &scale);
    // A null mappedFont is the documented "no font can render this" answer, not
    // an error: mappedLength is then the number of characters to skip.
    if (FAILED(hr) || mapped == nullptr || mappedLength == 0) {
        return std::nullopt;
    }

    const auto ref = fileRefOf(mapped.Get());
    if (!ref.has_value()) {
        return std::nullopt;
    }
    // scale is a multiplier on the returned font's em size, which is how
    // DirectWrite asks for a fallback that matches the base font's apparent
    // size. Ignoring it makes CJK fallbacks visibly the wrong size.
    const auto scaled = static_cast<int>(std::lround(static_cast<double>(pxHeight) * scale));
    return FaceSpec{
        .path = ref->path, .index = ref->index, .pxHeight = scaled > 0 ? scaled : pxHeight};
}

std::vector<std::uint32_t> shapeWithFallback(ShapePool& pool, const FontDb& fonts,
                                             std::span<const Run> runs, std::uint32_t primaryFaceId,
                                             std::string_view primaryFamily, int pxHeight,
                                             bool ligatures, std::vector<ShapedRun>& out) {
    std::vector<std::uint32_t> faces(runs.size(), primaryFaceId);
    if (!pool.shapeAll(runs, primaryFaceId, ligatures, out)) {
        return faces;  // timed out; the frame draws what it has
    }

    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (!out[i].missingGlyphs) {
            continue;
        }
        const bool bold = (runs[i].shaping & core::vt::Attr::kBold) != 0;
        const bool italic = (runs[i].shaping & core::vt::Attr::kItalic) != 0;
        const auto spec = fonts.fallbackFor(runs[i].text, primaryFamily, bold, italic, pxHeight);
        if (!spec.has_value()) {
            continue;  // nothing covers it; the .notdef boxes are the honest answer
        }
        const auto faceId = pool.registerFace(*spec);
        if (!faceId.has_value()) {
            continue;
        }

        // ponytail: ONE fallback hop, not an N-deep chain, and the whole run is
        // re-shaped with the mapped face rather than only the sub-span
        // MapCharacters covered. A terminal run is already split per script
        // (run_splitter.h), so one hop covers it; a run needing two different
        // fallback faces would keep .notdef for the second. Upgrade path: loop
        // on the still-missing sub-span. Do not build that before a real case.
        const std::array<Run, 1> single{runs[i]};
        std::vector<ShapedRun> reshaped;
        if (pool.shapeAll(single, *faceId, ligatures, reshaped) && !reshaped[0].glyphs.empty()) {
            out[i] = std::move(reshaped[0]);
            faces[i] = *faceId;
        }
    }
    return faces;
}

}  // namespace krait::render
