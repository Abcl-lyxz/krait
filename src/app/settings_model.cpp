#include "settings_model.h"

#include "settings/schema.h"

#include <QStringList>
#include <QVariantMap>

namespace krait::app {

namespace {

QString typeName(settings::Type type) {
    switch (type) {
    case settings::Type::Bool:
        return QStringLiteral("bool");
    case settings::Type::Int:
        return QStringLiteral("int");
    case settings::Type::String:
        break;
    }
    return QStringLiteral("string");
}

}  // namespace

SettingsModel::SettingsModel(QObject* parent) : QObject(parent) {
    refresh();
}

void SettingsModel::setRegistry(settings::Registry* registry) {
    m_registry = registry;
    if (m_registry != nullptr) {
        // A hot reload has to reach the open settings page too, or the values on
        // screen quietly stop being the values in the file.
        connect(m_registry, &settings::Registry::reloaded, this, [this] { refresh(); });
    }
    refresh();
}

void SettingsModel::setQuery(const QString& query) {
    if (m_query == query) {
        return;
    }
    m_query = query;
    emit queryChanged();
    refresh();
}

QString SettingsModel::path() const {
    return m_registry != nullptr ? m_registry->path() : QString();
}

bool SettingsModel::readOnly() const {
    return m_registry != nullptr && m_registry->isFromFuture();
}

void SettingsModel::refresh() {
    m_rows.clear();
    const std::string query = m_query.toStdString();

    for (const settings::Def& def : settings::definitions()) {
        if (!settings::matchesSearch(def, query)) {
            continue;
        }
        QVariantMap row;
        row["id"] = QString::fromUtf8(def.id.data(), static_cast<qsizetype>(def.id.size()));
        row["type"] = typeName(def.type);
        row["doc"] = QString::fromUtf8(def.doc.data(), static_cast<qsizetype>(def.doc.size()));
        row["min"] = static_cast<qlonglong>(def.min);
        row["max"] = static_cast<qlonglong>(def.max);

        // Space-separated in the schema because a constexpr table cannot hold a
        // container; split here so QML gets a list it can put in a combo box.
        const QString choices =
            QString::fromUtf8(def.choices.data(), static_cast<qsizetype>(def.choices.size()));
        row["choices"] =
            choices.isEmpty() ? QStringList{} : choices.split(u' ', Qt::SkipEmptyParts);

        if (m_registry != nullptr) {
            switch (def.type) {
            case settings::Type::Bool:
                row["value"] = m_registry->boolean(def.id);
                break;
            case settings::Type::Int:
                row["value"] = static_cast<qlonglong>(m_registry->integer(def.id));
                break;
            case settings::Type::String:
                row["value"] = QString::fromStdString(m_registry->text(def.id));
                break;
            }
        }
        m_rows.append(row);
    }
    emit rowsChanged();
}

bool SettingsModel::setValue(const QString& id, const QVariant& value) {
    if (m_registry == nullptr) {
        return false;
    }
    const std::string key = id.toStdString();
    const settings::Def* def = settings::find(key);
    if (def == nullptr) {
        return false;
    }

    // Converted to the SCHEMA's type rather than whatever QML happened to hand
    // over — a spin box gives a double, and storing 20.0 in an integer setting
    // is how a config file stops round-tripping.
    settings::Value typed;
    switch (def->type) {
    case settings::Type::Bool:
        typed = value.toBool();
        break;
    case settings::Type::Int:
        typed = static_cast<std::int64_t>(value.toLongLong());
        break;
    case settings::Type::String:
        typed = value.toString().toStdString();
        break;
    }

    if (!m_registry->set(key, typed)) {
        return false;  // out of range, or not one of the choices
    }
    refresh();
    return true;
}

bool SettingsModel::save() {
    if (m_registry == nullptr || readOnly()) {
        return false;
    }
    return m_registry->save();
}

}  // namespace krait::app
