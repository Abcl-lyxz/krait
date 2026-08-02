#pragma once

#include "theme/theme.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <span>
#include <vector>

namespace krait::app::theme {

// The live theme: the builtins, plus every *.toml in the user's themes
// directory, plus which one is current.
//
// A QObject for its signals and nothing else — QtCore only, no QtGui. The
// renderer and the QML chrome both need the current theme, and they live on
// opposite sides of a module boundary (the renderer must not know QColor
// exists; QML cannot bind to a std::uint32_t). One store, two thin adapters,
// so a theme change cannot reach one of them and not the other.
class ThemeStore : public QObject {
    Q_OBJECT

  public:
    explicit ThemeStore(QObject* parent = nullptr);

    // Where user themes live and where the editor saves. Rescans immediately.
    // A directory that does not exist is a first run, not an error.
    void setDirectory(const QString& dir);

    const QString& directory() const { return m_dir; }

    // Re-reads the directory, keeping the current selection by NAME. A theme
    // file edited on disk therefore takes effect without reselecting it, which
    // is what makes hand-editing a theme a usable workflow at all.
    void reload();

    std::span<const Theme> themes() const { return m_themes; }

    // Never null and never partial: an unresolvable name falls back to
    // builtins()[0], because a terminal with no colours is not a degraded
    // state, it is an unusable one.
    const Theme& current() const;

    const QString& currentName() const { return m_currentName; }

    // False when no theme is called that. The name is remembered anyway, so a
    // config naming a theme whose file is missing picks it up once the file
    // comes back rather than silently rewriting itself to the default.
    bool select(const QString& name);

    // The live editor's uncommitted state. Everything reading the store sees
    // the override until it is cleared or saved — that IS the live preview,
    // and it means the preview path and the real path are the same path.
    void setPreview(const Theme& theme);
    void clearPreview();

    bool hasPreview() const { return m_preview.has_value(); }

    // Writes into directory(). Returns an empty string on success, or why not.
    // Refuses to write over a BUILTIN's name — two themes called "default-dark"
    // is a bug report nobody can diagnose.
    QString save(const Theme& theme);

    // Reads one file in any supported format and saves every theme it holds.
    // `imported` collects the names. Returns "" or the reason it failed.
    QString importFile(const QString& path, QStringList* imported);

    // Non-fatal problems from the last reload(): one line per file that could
    // not be read. Surfaced as a banner, never a dialog (rules/ui.md).
    const QStringList& warnings() const { return m_warnings; }

  signals:
    // The COLOURS changed — a different theme, a preview edit, or a reload that
    // altered the current one. Everything that paints listens to this.
    void changed();

    // The LIST changed: a file appeared, an import landed. The gallery listens
    // to this; the renderer does not, because a new file nobody selected must
    // not cost a full-frame invalidate.
    void listChanged();

  private:
    void rebuild();

    QString m_dir;
    QString m_currentName;
    std::vector<Theme> m_themes;
    std::optional<Theme> m_preview;
    QStringList m_warnings;
};

}  // namespace krait::app::theme
