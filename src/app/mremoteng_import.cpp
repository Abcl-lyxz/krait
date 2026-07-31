#include "mremoteng_import.h"

#include <QCoreApplication>
#include <QStringList>
#include <QXmlStreamReader>

#include <utility>

namespace krait::app {

namespace {

// mRemoteNG's own deserializer refuses anything above this
// (MaxSupportedConfVersion). Refusing the same way beats guessing at a layout
// written by a version that did not exist yet.
constexpr double kMaxConfVersion = 2.8;

// See the note at the push site: the folder path and the id are rebuilt by
// joining at every leaf, so depth multiplies into the work per connection.
constexpr int kMaxFolderDepth = 64;

QString attribute(const QXmlStreamAttributes& attributes, const char* name) {
    return attributes.value(QLatin1String(name)).toString();
}

bool isTrue(const QString& text) {
    return text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
}

// mRemoteNG writes the enum NAME, e.g. Protocol="SSH2". The ones Krait has a
// backend for are the only ones that can become a profile; importing an RDP
// connection as SSH would produce something that fails at connect time with an
// error about the wrong protocol.
bool backendFor(const QString& protocol, session::BackendKind* kind) {
    if (protocol == QLatin1String("SSH2") || protocol == QLatin1String("SSH1")) {
        // SSH1 too. The profile is still an SSH host, and the version is not
        // ours to honour: ADR-0002's algorithm policy has no SSH1 in it, so the
        // connection is refused by our own crypto policy rather than silently
        // downgraded. A saved session that says why beats one that vanished.
        *kind = session::BackendKind::Ssh;
        return true;
    }
    if (protocol == QLatin1String("Telnet")) {
        *kind = session::BackendKind::Telnet;
        return true;
    }
    if (protocol == QLatin1String("RAW")) {
        *kind = session::BackendKind::Raw;
        return true;
    }
    return false;
}

std::int64_t defaultPortFor(session::BackendKind kind) {
    if (kind == session::BackendKind::Telnet) {
        return 23;
    }
    return 22;
}

// A container's values, for the children that say they inherit them. mRemoteNG
// has no InheritHostname — a hostname never inherits — so this carries only the
// three that do and that Krait has a field for.
struct Inherited {
    QString user;
    QString port;
    QString protocol;
};

}  // namespace

MremotengImport importFromMremoteng(const QString& xml) {
    MremotengImport result;
    QXmlStreamReader reader(xml);

    // The folder path is built from the Container names on the way down, which
    // is how mRemoteNG expresses a tree and how Krait expresses one too.
    QStringList folders;
    std::vector<Inherited> inherited;
    // One entry per OPEN Node element, saying whether it pushed a folder. The
    // XML nests connections inside containers, and only containers are folders.
    std::vector<bool> openNodes;
    bool sawRoot = false;

    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();

        if (token == QXmlStreamReader::EndElement) {
            // Every Node closes, including a self-closing `<Node ... />` — the
            // reader emits Start and End for those too. So the pop has to be
            // driven by what was PUSHED, not by seeing a Node end: a leaf
            // connection ending would otherwise pop its parent's folder, and
            // every sibling after it would import at the top level.
            if (reader.name() == QLatin1String("Node") && !openNodes.empty()) {
                const bool wasContainer = openNodes.back();
                openNodes.pop_back();
                if (wasContainer && !folders.isEmpty()) {
                    folders.removeLast();
                    inherited.pop_back();
                }
            }
            continue;
        }
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }

        const QXmlStreamAttributes attributes = reader.attributes();

        if (reader.name() == QLatin1String("Connections")) {
            sawRoot = true;
            // Refuse rather than half-import. The whole document body is
            // ciphertext when this is set, so there is nothing to parse without
            // the user's file password — and asking for that would mean holding
            // the key to a file of credentials we have already decided not to
            // import.
            if (isTrue(attribute(attributes, "FullFileEncryption"))) {
                result.error = QCoreApplication::translate(
                    "mremoteng",
                    "This connection file is fully encrypted, so Krait cannot read it. Saved "
                    "passwords are never imported in any case.");
                return result;
            }
            // Written with the invariant culture, but mRemoteNG itself replaces
            // a comma before parsing, which says files with one exist.
            const QString version =
                attribute(attributes, "ConfVersion").replace(QLatin1Char(','), QLatin1Char('.'));
            bool numeric = false;
            const double parsed = version.toDouble(&numeric);
            if (numeric && parsed > kMaxConfVersion) {
                result.error =
                    QCoreApplication::translate(
                        "mremoteng",
                        "This connection file is version %1, which is newer than Krait knows how "
                        "to read.")
                        .arg(version);
                return result;
            }
            continue;
        }

        // Node is in the EMPTY namespace while the root is in mrng:, which is
        // why this compares the local name and never a prefixed one.
        if (reader.name() != QLatin1String("Node")) {
            continue;
        }

        const QString name = attribute(attributes, "Name");
        // A missing Type means Connection, which is what mRemoteNG's own
        // deserializer defaults to. Deciding by "does it have a Hostname"
        // instead would be wrong: containers carry Hostname, Protocol and Port
        // too, because the serializer writes the full attribute set for both.
        const bool isContainer = attribute(attributes, "Type") == QLatin1String("Container");
        // Starts false and is set true only once a folder has ACTUALLY been
        // pushed. Recording the intent instead would desynchronise the stacks
        // the moment a container is refused below, and the pop on its
        // EndElement would take a folder belonging to its parent.
        openNodes.push_back(false);

        Inherited parent;
        if (!inherited.empty()) {
            parent = inherited.back();
        }
        const auto inheritedOr = [&attributes](const char* own, const char* flag,
                                               const QString& fromParent) {
            return isTrue(attribute(attributes, flag)) ? fromParent : attribute(attributes, own);
        };
        const QString user = inheritedOr("Username", "InheritUsername", parent.user);
        const QString port = inheritedOr("Port", "InheritPort", parent.port);
        const QString protocol = inheritedOr("Protocol", "InheritProtocol", parent.protocol);

        if (isContainer) {
            // Bounded, because the folder path and the id are rebuilt by
            // joining this list at every leaf. Deep nesting therefore costs
            // depth × leaves in string building, which a file with tens of
            // thousands of both turns into a frozen window rather than an
            // import. Nobody has a connection tree 64 deep.
            if (folders.size() >= kMaxFolderDepth) {
                result.skipped.append(
                    QCoreApplication::translate("mremoteng", "%1 (nested too deeply)").arg(name));
                continue;
            }
            folders.append(name);
            inherited.push_back(Inherited{user, port, protocol});
            openNodes.back() = true;
            continue;
        }

        // A Connection is a leaf: mRemoteNG does not nest inside one, so no
        // folder level is pushed for it. Pushing one would put every host in a
        // folder named after itself.
        session::BackendKind kind = session::BackendKind::Ssh;
        const QString host = attribute(attributes, "Hostname");
        if (!backendFor(protocol, &kind)) {
            result.skipped.append(
                QCoreApplication::translate("mremoteng", "%1 (%2)")
                    .arg(name, protocol.isEmpty()
                                   ? QCoreApplication::translate("mremoteng", "no protocol")
                                   : protocol));
            continue;
        }
        if (host.isEmpty()) {
            result.skipped.append(
                QCoreApplication::translate("mremoteng", "%1 (no hostname)").arg(name));
            continue;
        }

        session::Profile profile;
        profile.backend = kind;
        profile.markExplicit("backend");
        profile.name = name.toStdString();
        profile.markExplicit("name");
        profile.host = host.toStdString();
        profile.markExplicit("host");
        if (!folders.isEmpty()) {
            profile.folder = folders.join(QLatin1Char('/')).toStdString();
            profile.markExplicit("folder");
        }
        if (!user.isEmpty()) {
            profile.user = user.toStdString();
            profile.markExplicit("user");
        }

        bool numericPort = false;
        const int parsedPort = port.toInt(&numericPort);
        if (numericPort && parsedPort > 0 && parsedPort <= 65535) {
            profile.port = parsedPort;
        } else {
            // mRemoteNG stores the protocol's default rather than leaving it
            // empty, but a hand-edited file can hold anything, and a telnet
            // session that imported with port 22 would be a puzzle to debug.
            profile.port = defaultPortFor(kind);
        }
        profile.markExplicit("port");

        // The id comes from the FOLDER PATH and the name, not the name alone:
        // mRemoteNG lets two folders each hold a "web-1", and colliding slugs
        // would make add() rename the second one during the import.
        QStringList idParts = folders;
        idParts.append(name);
        profile.id = session::slugify(idParts.join(QLatin1Char('-')).toStdString());

        result.profiles.push_back(std::move(profile));
    }

    if (reader.hasError()) {
        result.error =
            QCoreApplication::translate("mremoteng", "Could not read the connection file: %1")
                .arg(reader.errorString());
        return result;
    }
    if (!sawRoot) {
        result.error = QCoreApplication::translate("mremoteng",
                                                   "That file is not a mRemoteNG connection file.");
    }
    return result;
}

}  // namespace krait::app
