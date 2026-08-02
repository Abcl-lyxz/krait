#include "theme_model.h"

#include "theme/theme.h"

#include <QByteArray>
#include <QVariantMap>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace krait::app {
namespace {

// Borrowed, set by main() before the QML engine runs. A file-scope pointer
// rather than a constructor argument because the engine constructs the
// singleton, and it does so during the first binding evaluation — there is no
// later moment at which anything could inject a dependency.
theme::ThemeStore* g_store = nullptr;

QColor toColor(theme::Rgb rgb) {
    return QColor::fromRgb(static_cast<QRgb>(rgb | 0xFF000000U));
}

// The editor's rows, in the order they are shown. `group` is the section
// header; `label` is what the user reads. Both stay as untranslated literals
// here and go through tr() at the call site — a table of QStrings would be
// built before the translator is installed and would never re-translate.
struct TokenRow {
    const char* key;
    const char* label;
    const char* group;
};

// Palette rows first: they are what somebody opening a theme editor came to
// change. Chrome is below because it is derived by default and most themes
// never touch it.
constexpr std::array<TokenRow, 6> kPaletteRows{{
    {"bg", QT_TRANSLATE_NOOP("ThemeModel", "Background"),
     QT_TRANSLATE_NOOP("ThemeModel", "Terminal")},
    {"fg", QT_TRANSLATE_NOOP("ThemeModel", "Foreground"),
     QT_TRANSLATE_NOOP("ThemeModel", "Terminal")},
    {"cursor", QT_TRANSLATE_NOOP("ThemeModel", "Cursor"),
     QT_TRANSLATE_NOOP("ThemeModel", "Terminal")},
    {"cursorText", QT_TRANSLATE_NOOP("ThemeModel", "Cursor text"),
     QT_TRANSLATE_NOOP("ThemeModel", "Terminal")},
    {"selection", QT_TRANSLATE_NOOP("ThemeModel", "Selection"),
     QT_TRANSLATE_NOOP("ThemeModel", "Terminal")},
    {"highlight", QT_TRANSLATE_NOOP("ThemeModel", "Search highlight"),
     QT_TRANSLATE_NOOP("ThemeModel", "Terminal")},
}};

constexpr std::array<const char*, 16> kAnsiLabels{
    QT_TRANSLATE_NOOP("ThemeModel", "Black"),
    QT_TRANSLATE_NOOP("ThemeModel", "Red"),
    QT_TRANSLATE_NOOP("ThemeModel", "Green"),
    QT_TRANSLATE_NOOP("ThemeModel", "Yellow"),
    QT_TRANSLATE_NOOP("ThemeModel", "Blue"),
    QT_TRANSLATE_NOOP("ThemeModel", "Magenta"),
    QT_TRANSLATE_NOOP("ThemeModel", "Cyan"),
    QT_TRANSLATE_NOOP("ThemeModel", "White"),
    QT_TRANSLATE_NOOP("ThemeModel", "Bright black"),
    QT_TRANSLATE_NOOP("ThemeModel", "Bright red"),
    QT_TRANSLATE_NOOP("ThemeModel", "Bright green"),
    QT_TRANSLATE_NOOP("ThemeModel", "Bright yellow"),
    QT_TRANSLATE_NOOP("ThemeModel", "Bright blue"),
    QT_TRANSLATE_NOOP("ThemeModel", "Bright magenta"),
    QT_TRANSLATE_NOOP("ThemeModel", "Bright cyan"),
    QT_TRANSLATE_NOOP("ThemeModel", "Bright white"),
};

constexpr std::array<TokenRow, 14> kChromeRows{{
    {"chrome.bg", QT_TRANSLATE_NOOP("ThemeModel", "Window"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.surface", QT_TRANSLATE_NOOP("ThemeModel", "Bars and panels"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.surfaceAlt", QT_TRANSLATE_NOOP("ThemeModel", "Buttons and tabs"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.overlay", QT_TRANSLATE_NOOP("ThemeModel", "Popups"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.border", QT_TRANSLATE_NOOP("ThemeModel", "Borders"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.selection", QT_TRANSLATE_NOOP("ThemeModel", "Selected row"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.text", QT_TRANSLATE_NOOP("ThemeModel", "Text"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.textDim", QT_TRANSLATE_NOOP("ThemeModel", "Secondary text"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.textFaint", QT_TRANSLATE_NOOP("ThemeModel", "Hint text"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.accent", QT_TRANSLATE_NOOP("ThemeModel", "Accent"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.success", QT_TRANSLATE_NOOP("ThemeModel", "Success"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.warning", QT_TRANSLATE_NOOP("ThemeModel", "Warning"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.danger", QT_TRANSLATE_NOOP("ThemeModel", "Danger"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
    {"chrome.scrim", QT_TRANSLATE_NOOP("ThemeModel", "Dimming"),
     QT_TRANSLATE_NOOP("ThemeModel", "Interface")},
}};

// The one key -> member map, shared by the reader and the writer for the same
// reason kChromeFields is: a token that can be shown and not set is a slider
// that does nothing.
theme::Rgb* locate(theme::Theme& into, QStringView key) {
    if (key == QLatin1String("bg")) {
        return &into.bg;
    }
    if (key == QLatin1String("fg")) {
        return &into.fg;
    }
    if (key == QLatin1String("cursor")) {
        return &into.cursor;
    }
    if (key == QLatin1String("cursorText")) {
        return &into.cursorText;
    }
    if (key == QLatin1String("selection")) {
        return &into.selectionBg;
    }
    if (key == QLatin1String("highlight")) {
        return &into.highlightBg;
    }
    if (key.startsWith(QLatin1String("ansi."))) {
        bool ok = false;
        const int index = key.sliced(5).toInt(&ok);
        if (!ok || index < 0 || index > 15) {
            return nullptr;
        }
        return &into.ansi[static_cast<std::size_t>(index)];
    }
    if (!key.startsWith(QLatin1String("chrome."))) {
        return nullptr;
    }
    const QStringView token = key.sliced(7);
    theme::Chrome& chrome = into.chrome;
    if (token == QLatin1String("bg")) {
        return &chrome.bg;
    }
    if (token == QLatin1String("surface")) {
        return &chrome.surface;
    }
    if (token == QLatin1String("surfaceAlt")) {
        return &chrome.surfaceAlt;
    }
    if (token == QLatin1String("overlay")) {
        return &chrome.overlay;
    }
    if (token == QLatin1String("border")) {
        return &chrome.border;
    }
    if (token == QLatin1String("selection")) {
        return &chrome.selection;
    }
    if (token == QLatin1String("text")) {
        return &chrome.text;
    }
    if (token == QLatin1String("textDim")) {
        return &chrome.textDim;
    }
    if (token == QLatin1String("textFaint")) {
        return &chrome.textFaint;
    }
    if (token == QLatin1String("accent")) {
        return &chrome.accent;
    }
    if (token == QLatin1String("success")) {
        return &chrome.success;
    }
    if (token == QLatin1String("warning")) {
        return &chrome.warning;
    }
    if (token == QLatin1String("danger")) {
        return &chrome.danger;
    }
    if (token == QLatin1String("scrim")) {
        return &chrome.scrim;
    }
    return nullptr;
}

QVariantMap tokenRow(const theme::Theme& live, const char* key, const QString& label,
                     const QString& group) {
    theme::Theme copy = live;
    const QString keyText = QString::fromLatin1(key);
    const theme::Rgb* value = locate(copy, keyText);
    QVariantMap row;
    row.insert(QStringLiteral("key"), keyText);
    row.insert(QStringLiteral("label"), label);
    row.insert(QStringLiteral("group"), group);
    row.insert(QStringLiteral("value"), toColor(value != nullptr ? *value : 0));
    row.insert(QStringLiteral("hex"),
               QString::fromStdString(theme::formatColor(value != nullptr ? *value : 0)));
    return row;
}

}  // namespace

ThemeModel::ThemeModel(QObject* parent) : QObject(parent) {
    if (g_store == nullptr) {
        return;
    }
    connect(g_store, &theme::ThemeStore::changed, this, &ThemeModel::changed);
    connect(g_store, &theme::ThemeStore::listChanged, this, &ThemeModel::galleryChanged);
}

void ThemeModel::setStore(theme::ThemeStore* store) {
    g_store = store;
}

const theme::Theme& ThemeModel::theme() const {
    // Without a store — the QML type is instantiable in a designer or a test
    // harness that never called setStore — the first builtin is the answer. A
    // null check per getter beats fourteen crashes.
    static const theme::Theme kFallback = theme::builtins().front();
    return g_store != nullptr ? g_store->current() : kFallback;
}

QString ThemeModel::name() const {
    return QString::fromStdString(theme().name);
}

bool ThemeModel::dark() const {
    return theme::isDark(theme());
}

QColor ThemeModel::bg() const {
    return toColor(theme().chrome.bg);
}

QColor ThemeModel::surface() const {
    return toColor(theme().chrome.surface);
}

QColor ThemeModel::surfaceAlt() const {
    return toColor(theme().chrome.surfaceAlt);
}

QColor ThemeModel::overlay() const {
    return toColor(theme().chrome.overlay);
}

QColor ThemeModel::border() const {
    return toColor(theme().chrome.border);
}

QColor ThemeModel::selection() const {
    return toColor(theme().chrome.selection);
}

QColor ThemeModel::text() const {
    return toColor(theme().chrome.text);
}

QColor ThemeModel::textDim() const {
    return toColor(theme().chrome.textDim);
}

QColor ThemeModel::textFaint() const {
    return toColor(theme().chrome.textFaint);
}

QColor ThemeModel::accent() const {
    return toColor(theme().chrome.accent);
}

QColor ThemeModel::success() const {
    return toColor(theme().chrome.success);
}

QColor ThemeModel::warning() const {
    return toColor(theme().chrome.warning);
}

QColor ThemeModel::danger() const {
    return toColor(theme().chrome.danger);
}

QColor ThemeModel::scrim() const {
    return toColor(theme().chrome.scrim);
}

QColor ThemeModel::ansi(int index) const {
    if (index < 0 || index > 15) {
        return {};
    }
    return toColor(theme().ansi[static_cast<std::size_t>(index)]);
}

QColor ThemeModel::wash(const QColor& color, qreal amount) const {
    const qreal blend = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
    const QColor base = bg();
    return QColor::fromRgbF(
        static_cast<float>(color.redF() + (base.redF() - color.redF()) * blend),
        static_cast<float>(color.greenF() + (base.greenF() - color.greenF()) * blend),
        static_cast<float>(color.blueF() + (base.blueF() - color.blueF()) * blend));
}

QVariantList ThemeModel::gallery() const {
    QVariantList rows;
    if (g_store == nullptr) {
        return rows;
    }
    const QString current = QString::fromStdString(theme().name);
    for (const theme::Theme& entry : g_store->themes()) {
        QVariantMap row;
        row.insert(QStringLiteral("name"), QString::fromStdString(entry.name));
        row.insert(QStringLiteral("source"), QString::fromStdString(entry.source));
        row.insert(QStringLiteral("builtin"), entry.source == "builtin");
        row.insert(QStringLiteral("dark"), theme::isDark(entry));
        row.insert(QStringLiteral("bg"), toColor(entry.bg));
        row.insert(QStringLiteral("fg"), toColor(entry.fg));
        row.insert(QStringLiteral("accent"), toColor(entry.chrome.accent));
        row.insert(QStringLiteral("current"), QString::fromStdString(entry.name) == current);
        QVariantList swatches;
        for (const theme::Rgb color : entry.ansi) {
            swatches.append(toColor(color));
        }
        row.insert(QStringLiteral("ansi"), swatches);
        rows.append(row);
    }
    return rows;
}

QVariantList ThemeModel::tokens() const {
    const theme::Theme& live = theme();
    QVariantList rows;
    for (const TokenRow& row : kPaletteRows) {
        rows.append(tokenRow(live, row.key, tr(row.label), tr(row.group)));
    }
    for (std::size_t i = 0; i < kAnsiLabels.size(); ++i) {
        const std::string key = "ansi." + std::to_string(i);
        rows.append(tokenRow(live, key.c_str(), tr(kAnsiLabels[i]), tr("Palette")));
    }
    for (const TokenRow& row : kChromeRows) {
        rows.append(tokenRow(live, row.key, tr(row.label), tr(row.group)));
    }
    return rows;
}

bool ThemeModel::edited() const {
    return g_store != nullptr && g_store->hasPreview();
}

QStringList ThemeModel::warnings() const {
    return g_store != nullptr ? g_store->warnings() : QStringList{};
}

bool ThemeModel::apply(const QString& name) {
    if (g_store == nullptr) {
        return false;
    }
    return g_store->select(name);
}

bool ThemeModel::setToken(const QString& key, const QString& color) {
    if (g_store == nullptr) {
        return false;
    }
    const QByteArray utf8 = color.toUtf8();
    const std::optional<theme::Rgb> parsed = theme::parseColor(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    if (!parsed) {
        return false;
    }
    theme::Theme working = g_store->current();
    theme::Rgb* slot = locate(working, key);
    if (slot == nullptr) {
        return false;
    }
    *slot = *parsed;
    g_store->setPreview(working);
    return true;
}

QString ThemeModel::saveAs(const QString& name) {
    if (g_store == nullptr) {
        return tr("The theme system is not running.");
    }
    theme::Theme working = g_store->current();
    working.name = name.trimmed().toStdString();
    working.source = "user";
    return g_store->save(working);
}

void ThemeModel::revert() {
    if (g_store != nullptr) {
        g_store->clearPreview();
    }
}

QString ThemeModel::importFile(const QString& path) {
    if (g_store == nullptr) {
        return tr("The theme system is not running.");
    }
    QStringList imported;
    const QString error = g_store->importFile(path, &imported);
    m_lastImported = imported.join(QStringLiteral(", "));
    return error;
}

}  // namespace krait::app
