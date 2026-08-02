#include "theme/store.h"

#include "theme/import.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <string>
#include <string_view>

namespace krait::app::theme {
namespace {

std::string_view viewOf(const QByteArray& bytes) {
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

}  // namespace

ThemeStore::ThemeStore(QObject* parent) : QObject(parent) {
    m_currentName = QString::fromStdString(builtins().front().name);
    rebuild();
}

void ThemeStore::setDirectory(const QString& dir) {
    m_dir = dir;
    reload();
}

void ThemeStore::rebuild() {
    m_themes.assign(builtins().begin(), builtins().end());
    m_warnings.clear();
    if (m_dir.isEmpty()) {
        return;
    }

    QDir dir(m_dir);
    if (!dir.exists()) {
        return;  // first run
    }
    const QStringList files = dir.entryList({QStringLiteral("*.toml")}, QDir::Files, QDir::Name);
    for (const QString& file : files) {
        const QString path = dir.filePath(file);
        QFile handle(path);
        if (!handle.open(QIODevice::ReadOnly)) {
            m_warnings << tr("%1: %2").arg(file, handle.errorString());
            continue;
        }
        const QByteArray bytes = handle.readAll();
        std::string error;
        std::optional<Theme> parsed = parseToml(viewOf(bytes), &error);
        if (!parsed) {
            m_warnings << tr("%1: %2").arg(file, QString::fromStdString(error));
            continue;
        }
        parsed->source = path.toStdString();
        // A file wins over a builtin of the same name. That is the ONLY way to
        // fix a builtin you dislike without a fork, and the file's path is in
        // source so the gallery can say which one is showing.
        const auto existing = std::ranges::find(m_themes, parsed->name, &Theme::name);
        if (existing == m_themes.end()) {
            m_themes.push_back(std::move(*parsed));
        } else {
            *existing = std::move(*parsed);
        }
    }
}

void ThemeStore::reload() {
    const Theme before = current();
    rebuild();
    emit listChanged();
    if (!(current() == before)) {
        emit changed();
    }
}

const Theme& ThemeStore::current() const {
    if (m_preview) {
        return *m_preview;
    }
    const Theme* found = find(m_themes, m_currentName.toStdString());
    return found != nullptr ? *found : builtins().front();
}

bool ThemeStore::select(const QString& name) {
    if (m_currentName == name && !m_preview) {
        return find(m_themes, name.toStdString()) != nullptr;
    }
    m_currentName = name;
    // Selecting always drops the preview: picking a different theme while the
    // editor holds an unsaved edit has exactly one sane reading, and it is not
    // "keep painting the other theme's colours".
    m_preview.reset();
    emit changed();
    return find(m_themes, name.toStdString()) != nullptr;
}

void ThemeStore::setPreview(const Theme& theme) {
    m_preview = theme;
    emit changed();
}

void ThemeStore::clearPreview() {
    if (!m_preview) {
        return;
    }
    m_preview.reset();
    emit changed();
}

QString ThemeStore::save(const Theme& theme) {
    if (m_dir.isEmpty()) {
        return tr("No themes directory is configured.");
    }
    if (theme.name.empty()) {
        return tr("A theme needs a name.");
    }
    // A builtin's name is reserved. Overwriting one from the editor would leave
    // the user with two entries called the same thing and no way to tell which
    // the terminal is wearing.
    if (find(builtins(), theme.name) != nullptr) {
        return tr("\"%1\" is a built-in theme. Save it under a different name.")
            .arg(QString::fromStdString(theme.name));
    }

    QDir dir(m_dir);
    if (!dir.exists() && !QDir().mkpath(m_dir)) {
        return tr("Could not create %1.").arg(m_dir);
    }
    const QString path = dir.filePath(QString::fromStdString(themeFileName(theme.name)));

    // QSaveFile: a theme half-written because the disk filled is a theme that
    // fails to parse on the next start, and the file it replaced is gone.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return tr("%1: %2").arg(path, file.errorString());
    }
    const std::string text = toToml(theme);
    if (file.write(text.data(), static_cast<qint64>(text.size())) !=
            static_cast<qint64>(text.size()) ||
        !file.commit()) {
        return tr("%1: %2").arg(path, file.errorString());
    }

    rebuild();
    emit listChanged();
    // Saving is what commits a preview: the editor's uncommitted state becomes
    // the file, so the override has to go or the store would keep painting a
    // copy of something that now exists on disk.
    m_preview.reset();
    m_currentName = QString::fromStdString(theme.name);
    emit changed();
    return {};
}

QString ThemeStore::importFile(const QString& path, QStringList* imported) {
    QFile handle(path);
    if (!handle.open(QIODevice::ReadOnly)) {
        return tr("%1: %2").arg(QFileInfo(path).fileName(), handle.errorString());
    }
    // 4 MiB. A colour scheme is kilobytes; anything past this is a file that
    // was picked by mistake, and reading it whole first is how a file picker
    // aimed at a disk image freezes the UI thread.
    constexpr qint64 kMaxBytes = 4LL * 1024 * 1024;
    if (handle.size() > kMaxBytes) {
        return tr("%1 is too large to be a colour scheme.").arg(QFileInfo(path).fileName());
    }
    const QByteArray bytes = handle.readAll();

    const QByteArray nameUtf8 = QFileInfo(path).fileName().toUtf8();
    std::string error;
    std::vector<Theme> found = importAny(viewOf(nameUtf8), viewOf(bytes), &error);
    if (found.empty()) {
        return error.empty() ? tr("No colour scheme in this file.") : QString::fromStdString(error);
    }

    for (Theme& theme : found) {
        // An imported name that collides with a builtin gets suffixed rather
        // than refused: a Windows Terminal settings.json really does ship a
        // scheme called "Solarized Dark", and refusing the whole import over a
        // name is the wrong answer.
        if (find(builtins(), theme.name) != nullptr) {
            theme.name += " (imported)";
        }
        if (const QString failure = save(theme); !failure.isEmpty()) {
            return failure;
        }
        if (imported != nullptr) {
            *imported << QString::fromStdString(theme.name);
        }
    }
    return {};
}

}  // namespace krait::app::theme
