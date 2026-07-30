#pragma once

#include "settings/registry.h"

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace krait::app {

// The view-model behind the settings page.
//
// rules/ui.md: the schema is the single declaration and the UI is GENERATED
// from it. There is no per-setting QML and no hand-written list of controls —
// add a setting to schema.cpp and it appears here, searchable in both locales,
// with the right editor for its type. A settings page you have to edit twice is
// a settings page that will eventually disagree with its own file.
class SettingsModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(QString path READ path NOTIFY rowsChanged)
    // True when the file on disk was written by a NEWER schema than this build.
    // Saving is refused in that state, and the UI has to say so rather than
    // silently dropping whatever the newer version added.
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY rowsChanged)

  public:
    explicit SettingsModel(QObject* parent = nullptr);  // owned by parent

    // Borrowed; main() owns the live registry so every terminal and this page
    // read the same values and a hot reload reaches all of them.
    void setRegistry(settings::Registry* registry);

    const QString& query() const { return m_query; }

    void setQuery(const QString& query);

    const QVariantList& rows() const { return m_rows; }

    QString path() const;
    bool readOnly() const;

    // False when the value is not legal for that setting — the registry
    // validates against the schema and refuses rather than clamping, so the UI
    // can say "no" instead of silently storing something else.
    Q_INVOKABLE bool setValue(const QString& id, const QVariant& value);

    // Writes the file. False when the registry is refusing to save.
    Q_INVOKABLE bool save();

  signals:
    void queryChanged();
    void rowsChanged();

  private:
    void refresh();

    settings::Registry* m_registry = nullptr;  // borrowed; owned by main()
    QString m_query;
    QVariantList m_rows;
};

}  // namespace krait::app
