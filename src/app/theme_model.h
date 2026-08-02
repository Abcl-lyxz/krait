#pragma once

#include "theme/store.h"
#include "theme/theme.h"

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace krait::app {

// The QML face of the theme system: a singleton every .qml file reads its
// colours from.
//
// rules/ui.md: "Theme tokens (colors, spacing, radii) come from the theme
// system; hex literals in QML are a defect." A singleton rather than a property
// threaded down the tree, because the alternative is every component taking a
// `theme` property and every call site passing it — which is exactly the shape
// that leaves one popup still painted in the last theme's colours.
//
// The gallery and the live editor are here too rather than in a second model:
// they read and write the same store, and splitting them would mean two objects
// racing to own "what is the uncommitted edit".
class ThemeModel : public QObject {
    Q_OBJECT
    // Named `Theme` in QML rather than `ThemeModel`: it is read on nearly every
    // line of every .qml file (`color: Theme.surface`), and the shorter name is
    // what keeps a token cheaper to type than a hex literal — which is the only
    // way a "no hex literals" rule survives contact with the next feature.
    QML_NAMED_ELEMENT(Theme)
    QML_SINGLETON

    Q_PROPERTY(QString name READ name NOTIFY changed)
    // Whether the CURRENT theme reads as dark. QML uses it to pick icon sets
    // and shadow strength — the two things a colour token cannot express.
    Q_PROPERTY(bool dark READ dark NOTIFY changed)

    Q_PROPERTY(QColor bg READ bg NOTIFY changed)
    Q_PROPERTY(QColor surface READ surface NOTIFY changed)
    Q_PROPERTY(QColor surfaceAlt READ surfaceAlt NOTIFY changed)
    Q_PROPERTY(QColor overlay READ overlay NOTIFY changed)
    Q_PROPERTY(QColor border READ border NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor text READ text NOTIFY changed)
    Q_PROPERTY(QColor textDim READ textDim NOTIFY changed)
    Q_PROPERTY(QColor textFaint READ textFaint NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor success READ success NOTIFY changed)
    Q_PROPERTY(QColor warning READ warning NOTIFY changed)
    Q_PROPERTY(QColor danger READ danger NOTIFY changed)
    Q_PROPERTY(QColor scrim READ scrim NOTIFY changed)

    // One row per theme: { name, source, dark, bg, fg, accent, current }.
    Q_PROPERTY(QVariantList gallery READ gallery NOTIFY galleryChanged)
    // One row per editable colour: { key, label, group, value }.
    Q_PROPERTY(QVariantList tokens READ tokens NOTIFY changed)
    // True while the editor holds changes that are not on disk.
    Q_PROPERTY(bool edited READ edited NOTIFY changed)
    // Non-fatal theme-file problems, for a banner. Never a dialog.
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY galleryChanged)

  public:
    explicit ThemeModel(QObject* parent = nullptr);  // owned by parent

    // Handed over BEFORE the QML engine builds anything — the singleton is
    // constructed by the engine on first use, which is during the first
    // binding evaluation, so there is no later moment to inject into.
    // Borrowed; main() owns the store.
    static void setStore(theme::ThemeStore* store);

    QString name() const;
    bool dark() const;

    QColor bg() const;
    QColor surface() const;
    QColor surfaceAlt() const;
    QColor overlay() const;
    QColor border() const;
    QColor selection() const;
    QColor text() const;
    QColor textDim() const;
    QColor textFaint() const;
    QColor accent() const;
    QColor success() const;
    QColor warning() const;
    QColor danger() const;
    QColor scrim() const;

    QVariantList gallery() const;
    QVariantList tokens() const;
    bool edited() const;
    QStringList warnings() const;

    // The ANSI colour at `index`, for the gallery's preview strip. Out of range
    // returns an invalid QColor rather than clamping — a swatch that silently
    // shows the wrong colour is worse than one that does not draw.
    Q_INVOKABLE QColor ansi(int index) const;

    // A token blended `amount` of the way toward the window background. This is
    // what the banner tints and the armed-broadcast bar are built from, and it
    // is a function rather than six more tokens because the answer has to move
    // with the theme: a fixed dark-red banner background is unreadable the
    // moment somebody picks a light theme.
    Q_INVOKABLE QColor wash(const QColor& color, qreal amount) const;

    // Selects a theme and PERSISTS it to theme.name. False when nothing is
    // called that.
    Q_INVOKABLE bool apply(const QString& name);

    // Live editing. setToken repaints immediately and saves nothing; saveAs()
    // writes the working copy under `name` and selects it; revert() throws the
    // working copy away. Keys are "chrome.accent", "ansi.3", "fg", "bg",
    // "cursor", "cursorText", "selection", "highlight".
    Q_INVOKABLE bool setToken(const QString& key, const QString& color);
    Q_INVOKABLE QString saveAs(const QString& name);
    Q_INVOKABLE void revert();

    // Returns "" or the reason it failed. The imported names go to the banner,
    // because an import that silently succeeds leaves the user hunting the
    // gallery for a name they never saw.
    Q_INVOKABLE QString importFile(const QString& path);

    Q_INVOKABLE QString lastImported() const { return m_lastImported; }

  signals:
    void changed();
    void galleryChanged();

  private:
    const theme::Theme& theme() const;

    QString m_lastImported;
};

}  // namespace krait::app
