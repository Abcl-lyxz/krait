// T62: the mRemoteNG confCons.xml importer.
//
// The three things that bite a parser written from a glance at the file, all
// asserted here: `Node` is in the EMPTY namespace while the root is in `mrng:`;
// containers carry Hostname, Protocol and Port too, so `Type` is the only way
// to tell them apart; and `Type` may be absent, meaning Connection.
//
// The password attribute appears in these fixtures on purpose. It is there so
// there is something to fail against if importing one ever starts.

#include "mremoteng_import.h"
#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>

#include <string>

using krait::app::importFromMremoteng;
using krait::app::MremotengImport;
using krait::app::session::BackendKind;
using krait::app::session::Profile;

namespace {

// A whole file, so the root attributes are exercised rather than assumed.
QString wrap(const QString& nodes, const QString& rootAttributes = QString()) {
    return QStringLiteral(
               R"(<?xml version="1.0" encoding="utf-8"?>
<mrng:Connections xmlns:mrng="http://mremoteng.org" Name="Connections" Export="false"
  EncryptionEngine="AES" BlockCipherMode="GCM" KdfIterations="1000" ConfVersion="2.6" %1>
%2
</mrng:Connections>
)")
        .arg(rootAttributes, nodes);
}

const Profile* byName(const MremotengImport& imported, const char* name) {
    for (const Profile& profile : imported.profiles) {
        if (profile.name == name) {
            return &profile;
        }
    }
    return nullptr;
}

QString skippedText(const MremotengImport& imported) {
    QStringList lines;
    for (const QString& one : imported.skipped) {
        lines.append(one);
    }
    return lines.join(QStringLiteral(" "));
}

}  // namespace

TEST_CASE("a connection node becomes a profile", "[mremoteng]") {
    const MremotengImport imported = importFromMremoteng(wrap(QStringLiteral(
        R"(<Node Name="web1" Type="Connection" Hostname="10.0.0.1" Protocol="SSH2"
              Username="deploy" Port="2222" Password="Zm9vYmFy" />)")));
    CHECK(imported.error.isEmpty());
    REQUIRE(imported.profiles.size() == 1);
    const Profile& web = imported.profiles.front();
    CHECK(web.name == "web1");
    CHECK(web.host == "10.0.0.1");
    CHECK(web.user == "deploy");
    CHECK(web.port == 2222);
    CHECK(web.backend == BackendKind::Ssh);
}

TEST_CASE("no password ever reaches a profile", "[mremoteng]") {
    // Profile has nowhere to put one — the vault does, keyed by id — and that
    // is deliberate. This asserts the decision rather than the type: if a
    // password field is ever added to Profile, this is what should fail.
    const MremotengImport imported = importFromMremoteng(wrap(QStringLiteral(
        R"(<Node Name="web1" Type="Connection" Hostname="10.0.0.1" Protocol="SSH2"
              Password="aEWNFV5uGcjUHF0uS17QTdT9kVqtKCPe" VNCProxyPassword="x"
              RDGatewayPassword="y" />)")));
    REQUIRE(imported.profiles.size() == 1);
    const Profile& web = imported.profiles.front();
    const std::string everything = web.host + web.user + web.name + web.keyPath + web.certPath;
    CHECK(everything.find("aEWNFV") == std::string::npos);
}

TEST_CASE("containers become folders and connections nest under them", "[mremoteng]") {
    const MremotengImport imported = importFromMremoteng(wrap(QStringLiteral(
        R"(<Node Name="prod" Type="Container" Expanded="true" Hostname="" Protocol="RDP">
             <Node Name="eu" Type="Container" Hostname="" Protocol="RDP">
               <Node Name="web1" Type="Connection" Hostname="10.0.0.1" Protocol="SSH2" />
             </Node>
             <Node Name="db1" Type="Connection" Hostname="10.0.0.2" Protocol="SSH2" />
           </Node>
           <Node Name="loose" Type="Connection" Hostname="10.0.0.3" Protocol="SSH2" />)")));
    REQUIRE(imported.profiles.size() == 3);
    REQUIRE(byName(imported, "web1") != nullptr);
    CHECK(byName(imported, "web1")->folder == "prod/eu");
    CHECK(byName(imported, "db1")->folder == "prod");
    CHECK(byName(imported, "loose")->folder.empty());
    // The containers themselves carried Protocol="RDP" and an empty Hostname,
    // and neither turned into a profile OR a skip. Deciding by Type, not by
    // "does it have a hostname", is what makes that work.
    CHECK(imported.skipped.empty());
}

TEST_CASE("ids include the folder path so two folders may hold the same name", "[mremoteng]") {
    const MremotengImport imported = importFromMremoteng(wrap(QStringLiteral(
        R"(<Node Name="prod" Type="Container">
             <Node Name="web1" Type="Connection" Hostname="10.0.0.1" Protocol="SSH2" />
           </Node>
           <Node Name="staging" Type="Container">
             <Node Name="web1" Type="Connection" Hostname="10.9.9.1" Protocol="SSH2" />
           </Node>)")));
    REQUIRE(imported.profiles.size() == 2);
    // Colliding ids would make ProfileStore::add rename the second one during
    // the import, which reads as a bug in the importer.
    CHECK(imported.profiles[0].id != imported.profiles[1].id);
}

TEST_CASE("a missing Type means Connection", "[mremoteng]") {
    // mRemoteNG's own deserializer defaults to Connection. Treating a missing
    // Type as a container would drop the node and everything under it.
    const MremotengImport imported = importFromMremoteng(
        wrap(QStringLiteral(R"(<Node Name="web1" Hostname="10.0.0.1" Protocol="SSH2" />)")));
    REQUIRE(imported.profiles.size() == 1);
    CHECK(imported.profiles.front().name == "web1");
}

TEST_CASE("Inherit* takes the value from the enclosing container", "[mremoteng]") {
    const MremotengImport imported = importFromMremoteng(wrap(QStringLiteral(
        R"(<Node Name="prod" Type="Container" Username="deploy" Port="2222" Protocol="SSH2">
             <Node Name="inherits" Type="Connection" Hostname="10.0.0.1" Username="wrong"
                   Port="9999" Protocol="RDP" InheritUsername="true" InheritPort="true"
                   InheritProtocol="true" />
             <Node Name="its-own" Type="Connection" Hostname="10.0.0.2" Username="ops"
                   Port="2022" Protocol="SSH2" />
           </Node>)")));
    REQUIRE(imported.profiles.size() == 2);
    const Profile* inherits = byName(imported, "inherits");
    REQUIRE(inherits != nullptr);
    CHECK(inherits->user == "deploy");
    CHECK(inherits->port == 2222);
    // Protocol inherited too, which is what stops a node marked RDP-but-
    // inheriting from being skipped as an unsupported protocol.
    CHECK(inherits->backend == BackendKind::Ssh);
    REQUIRE(byName(imported, "its-own") != nullptr);
    CHECK(byName(imported, "its-own")->user == "ops");
    CHECK(byName(imported, "its-own")->port == 2022);
}

TEST_CASE("protocols without a backend are named, not silently dropped", "[mremoteng]") {
    const MremotengImport imported = importFromMremoteng(wrap(QStringLiteral(
        R"(<Node Name="desk" Type="Connection" Hostname="10.0.0.1" Protocol="RDP" />
           <Node Name="screen" Type="Connection" Hostname="10.0.0.2" Protocol="VNC" />
           <Node Name="shell" Type="Connection" Hostname="10.0.0.3" Protocol="SSH2" />
           <Node Name="old" Type="Connection" Hostname="10.0.0.4" Protocol="Telnet" />
           <Node Name="wire" Type="Connection" Hostname="10.0.0.5" Protocol="RAW" />)")));
    REQUIRE(imported.profiles.size() == 3);
    REQUIRE(byName(imported, "old") != nullptr);
    CHECK(byName(imported, "old")->backend == BackendKind::Telnet);
    // The telnet default, not SSH's 22: a telnet session that imported with
    // port 22 would be a puzzle to debug.
    CHECK(byName(imported, "old")->port == 23);
    CHECK(byName(imported, "wire")->backend == BackendKind::Raw);
    REQUIRE(imported.skipped.size() == 2);
    CHECK(skippedText(imported).contains(QStringLiteral("desk")));
    CHECK(skippedText(imported).contains(QStringLiteral("VNC")));
}

TEST_CASE("a fully encrypted file is refused with a reason", "[mremoteng]") {
    // The body is ciphertext, so there is nothing to parse — and asking for the
    // file password would mean holding the key to a file of credentials we have
    // already decided not to import.
    const MremotengImport imported = importFromMremoteng(
        wrap(QStringLiteral(R"(<Node Name="x" Type="Connection" Hostname="h" Protocol="SSH2" />)"),
             QStringLiteral(R"(FullFileEncryption="true")")));
    CHECK(imported.profiles.empty());
    CHECK_FALSE(imported.error.isEmpty());
}

TEST_CASE("a version newer than the known layout is refused", "[mremoteng]") {
    const QString xml = QStringLiteral(
        R"(<?xml version="1.0"?><mrng:Connections xmlns:mrng="http://mremoteng.org"
             ConfVersion="9.9"><Node Name="x" Type="Connection" Hostname="h"
             Protocol="SSH2" /></mrng:Connections>)");
    const MremotengImport imported = importFromMremoteng(xml);
    CHECK(imported.profiles.empty());
    CHECK_FALSE(imported.error.isEmpty());
}

TEST_CASE("something that is not a connection file is refused", "[mremoteng]") {
    CHECK_FALSE(
        importFromMremoteng(QStringLiteral("<html><body>nope</body></html>")).error.isEmpty());
    // Malformed XML reaches the reader's own error rather than a partial
    // import: half a connection list is worse than none.
    CHECK_FALSE(importFromMremoteng(QStringLiteral("<mrng:Connections><Node")).error.isEmpty());
    CHECK_FALSE(importFromMremoteng(QString()).error.isEmpty());
}

TEST_CASE("a connection with no hostname is named, not imported", "[mremoteng]") {
    const MremotengImport imported = importFromMremoteng(
        wrap(QStringLiteral(R"(<Node Name="empty" Type="Connection" Protocol="SSH2" />)")));
    CHECK(imported.profiles.empty());
    REQUIRE(imported.skipped.size() == 1);
    CHECK(imported.skipped.front().contains(QStringLiteral("empty")));
}
