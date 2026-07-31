// T30 — the settings registry. The plan's verification is "round-trip +
// migration + hot-reload test", and each of those is a way a config file can
// eat a user's configuration rather than a feature to demo.

#include "app/settings/paths.h"
#include "app/settings/registry.h"
#include "app/settings/schema.h"
#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

using krait::app::settings::Def;
using krait::app::settings::defaultValue;
using krait::app::settings::definitions;
using krait::app::settings::find;
using krait::app::settings::kSchemaVersion;
using krait::app::settings::Registry;
using krait::app::settings::validate;

namespace {

void writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(contents);
    file.close();
}

}  // namespace

TEST_CASE("every setting has a default its own schema accepts", "[settings]") {
    // A default outside its own range would mean a fresh install starts invalid
    // and the very first save writes something load() then rejects.
    for (const Def& def : definitions()) {
        INFO("setting: " << def.id);
        CHECK(validate(def, defaultValue(def)));
        // rules/ui.md: EN+TH search keywords are part of the declaration. Thai
        // ships as a first-class locale, and a translated label a Thai speaker
        // cannot SEARCH for in Thai is half a locale.
        CHECK_FALSE(def.searchEn.empty());
        CHECK_FALSE(def.searchTh.empty());
        CHECK_FALSE(def.doc.empty());
    }
}

TEST_CASE("a missing file is a first run, not a failure", "[settings]") {
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Registry registry;
    CHECK(registry.load(dir.filePath("nope.toml")));
    // Every setting still has a value: there is no state where a read fails.
    CHECK(registry.integer("font.size") == 20);
    CHECK(registry.text("gpu.adapter") == "auto");
}

TEST_CASE("settings round-trip through the file", "[settings]") {
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");

    Registry writer;
    REQUIRE(writer.load(path));
    REQUIRE(writer.set("font.size", std::int64_t{14}));
    REQUIRE(writer.set("font.family", std::string{"Cascadia Code"}));
    REQUIRE(writer.set("font.ligatures", true));
    REQUIRE(writer.set("unicode.eastAsianAmbiguous", std::string{"wide"}));
    REQUIRE(writer.save());

    Registry reader;
    REQUIRE(reader.load(path));
    CHECK(reader.integer("font.size") == 14);
    CHECK(reader.text("font.family") == "Cascadia Code");
    CHECK(reader.boolean("font.ligatures"));
    CHECK(reader.text("unicode.eastAsianAmbiguous") == "wide");
}

TEST_CASE("a saved file is readable and complete", "[settings]") {
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");
    Registry registry;
    REQUIRE(registry.load(path));
    REQUIRE(registry.save());

    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray text = file.readAll();
    // Every setting is written, including the untouched ones: this file is
    // meant to be read and edited by hand, and one that lists only what was
    // changed tells the user nothing about what they COULD change.
    CHECK(text.contains("schema_version"));
    CHECK(text.contains("[font]"));
    CHECK(text.contains("size"));
    CHECK(text.contains("[scrollback]"));
    CHECK(text.contains("[unicode]"));
}

TEST_CASE("an out-of-range value falls back rather than being clamped", "[settings]") {
    // Clamping 5000 to 200 silently gives the user a font size they did not ask
    // for and cannot tell they did not get.
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");
    writeFile(path, "[font]\nsize = 5000\n");

    Registry registry;
    REQUIRE(registry.load(path));
    CHECK(registry.integer("font.size") == 20);

    // ...and set() refuses it outright rather than storing something else.
    CHECK_FALSE(registry.set("font.size", std::int64_t{5000}));
    CHECK_FALSE(registry.set("font.size", std::int64_t{0}));
    CHECK(registry.set("font.size", std::int64_t{6}));
}

TEST_CASE("a wrong type in the file falls back", "[settings]") {
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");
    writeFile(path, "[font]\nsize = \"large\"\nligatures = 3\n");

    Registry registry;
    REQUIRE(registry.load(path));
    CHECK(registry.integer("font.size") == 20);
    CHECK_FALSE(registry.boolean("font.ligatures"));
}

TEST_CASE("a value outside the choices is refused", "[settings]") {
    Registry registry;
    CHECK(registry.set("gpu.adapter", std::string{"warp"}));
    CHECK_FALSE(registry.set("gpu.adapter", std::string{"warp2"}));  // not a prefix match
    CHECK_FALSE(registry.set("gpu.adapter", std::string{"vulkan"}));
    CHECK(registry.text("gpu.adapter") == "warp");
}

TEST_CASE("an unparseable file degrades to defaults instead of aborting", "[settings]") {
    // A config file is user input. Half a TOML table must not take the app down
    // on launch, leaving the user with no way in to fix it.
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");
    writeFile(path, "[font\nsize = ");

    Registry registry;
    CHECK_FALSE(registry.load(path));            // reported...
    CHECK(registry.integer("font.size") == 20);  // ...but still usable
}

TEST_CASE("migration: an unversioned file loads and is stamped on save", "[settings]") {
    // The v0 -> v1 story. A file written before schema_version existed is
    // treated as current, and the stamp appears the next time it is saved.
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");
    writeFile(path, "[font]\nsize = 18\n");

    Registry registry;
    REQUIRE(registry.load(path));
    CHECK(registry.integer("font.size") == 18);
    CHECK(registry.fileVersion() == kSchemaVersion);
    CHECK_FALSE(registry.isFromFuture());
    REQUIRE(registry.save());

    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll().contains("schema_version"));
}

TEST_CASE("migration: a file from a newer schema is never overwritten", "[settings]") {
    // Someone runs a newer Krait, then downgrades for an afternoon. Saving over
    // their config would strip whatever the newer version added — the settings
    // this build knows about would survive and everything else would be gone.
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");
    const QByteArray original = "schema_version = 99\n[font]\nsize = 18\nfuture_thing = true\n";
    writeFile(path, original);

    Registry registry;
    REQUIRE(registry.load(path));
    CHECK(registry.fileVersion() == 99);
    CHECK(registry.isFromFuture());
    CHECK(registry.integer("font.size") == 18);  // what it understands still loads
    CHECK_FALSE(registry.save());                // but it will not write over it

    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == original);
}

TEST_CASE("hot reload picks up an edit and reports what changed", "[settings]") {
    // reload() rather than the watcher: a test that waits on a filesystem
    // notification is a test that fails on a busy machine. The watcher is glue
    // around this call.
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");
    writeFile(path, "[font]\nsize = 14\n");

    Registry registry;
    REQUIRE(registry.load(path));
    REQUIRE(registry.integer("font.size") == 14);

    const QSignalSpy changed(&registry, &Registry::changed);
    const QSignalSpy reloaded(&registry, &Registry::reloaded);

    writeFile(path, "[font]\nsize = 22\nligatures = true\n");
    REQUIRE(registry.reload());

    CHECK(registry.integer("font.size") == 22);
    CHECK(registry.boolean("font.ligatures"));
    // One changed() per setting that actually moved, so a subsystem can react
    // to its own setting instead of rebuilding the world.
    CHECK(changed.count() == 2);
    CHECK(reloaded.count() == 1);
}

TEST_CASE("hot reload restores a default when a key is deleted", "[settings]") {
    // Without re-applying defaults first, deleting a key would leave the old
    // value in memory: the setting would look unchanged until the next restart,
    // which is the most confusing possible outcome for someone editing the file
    // to try something out.
    const QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("krait.toml");
    writeFile(path, "[font]\nsize = 30\n");

    Registry registry;
    REQUIRE(registry.load(path));
    REQUIRE(registry.integer("font.size") == 30);

    writeFile(path, "[font]\n");
    REQUIRE(registry.reload());
    CHECK(registry.integer("font.size") == 20);
}

TEST_CASE("setting the same value again emits nothing", "[settings]") {
    Registry registry;
    const QSignalSpy changed(&registry, &Registry::changed);
    REQUIRE(registry.set("font.size", std::int64_t{14}));
    CHECK(changed.count() == 1);
    REQUIRE(registry.set("font.size", std::int64_t{14}));
    CHECK(changed.count() == 1);  // listeners rebuild on every signal
}

TEST_CASE("an unknown id is refused, not invented", "[settings]") {
    Registry registry;
    CHECK(find("font.size") != nullptr);
    CHECK(find("font.colour") == nullptr);
    CHECK_FALSE(registry.set("font.colour", std::string{"red"}));
}

// ---- T34: the config-directory resolution order ----------------------------
//
// The plan's verification is "unit: resolution order table", and this is that
// table. Every input is injected, so the order is asserted without a real
// install, a real filesystem, or an environment variable set in one test that
// leaks into the next.

TEST_CASE("config resolution: the order table", "[settings][paths]") {
    using krait::app::settings::ConfigSource;
    using krait::app::settings::PathInputs;
    using krait::app::settings::portableMarkerName;
    using krait::app::settings::resolveConfigDir;

    const PathInputs all{.envOverride = "/env", .exeDir = "/exe", .userConfigDir = "/profile"};
    const auto markerPresent = [](const QString&) { return true; };
    const auto noMarker = [](const QString&) { return false; };

    SECTION("the environment override wins over everything") {
        const auto resolved = resolveConfigDir(all, markerPresent);
        CHECK(resolved.dir == "/env");
        CHECK(resolved.source == ConfigSource::EnvOverride);
    }

    SECTION("portable wins over the user profile") {
        PathInputs inputs = all;
        inputs.envOverride.clear();
        const auto resolved = resolveConfigDir(inputs, markerPresent);
        CHECK(resolved.dir == "/exe");
        CHECK(resolved.source == ConfigSource::Portable);
    }

    SECTION("without a marker it is the user profile, even next to the exe") {
        // A MARKER decides portable mode, not the presence of a config file.
        // An installed copy in a writable directory would otherwise become
        // portable the first time it saved, orphaning the real config.
        PathInputs inputs = all;
        inputs.envOverride.clear();
        const auto resolved = resolveConfigDir(inputs, noMarker);
        CHECK(resolved.dir == "/profile");
        CHECK(resolved.source == ConfigSource::UserProfile);
    }

    SECTION("the override is honoured even when it does not exist yet") {
        // Falling through to the profile here would silently ignore an explicit
        // instruction — the one thing an override must never do.
        PathInputs inputs{.envOverride = "/nowhere", .exeDir = "/exe", .userConfigDir = "/profile"};
        const auto resolved = resolveConfigDir(inputs, noMarker);
        CHECK(resolved.dir == "/nowhere");
        CHECK(resolved.source == ConfigSource::EnvOverride);
    }

    SECTION("the marker is looked for beside the executable, by name") {
        PathInputs inputs = all;
        inputs.envOverride.clear();
        QString probed;
        const auto record = [&probed](const QString& path) {
            probed = path;
            return false;
        };
        resolveConfigDir(inputs, record);
        CHECK(probed.contains(portableMarkerName()));
        CHECK(probed.startsWith("/exe"));
    }
}

TEST_CASE("the config file sits inside the resolved directory", "[settings][paths]") {
    using krait::app::settings::configFilePath;
    const QString path = configFilePath("/somewhere");
    CHECK(path.startsWith("/somewhere"));
    CHECK(path.endsWith("krait.toml"));
}

TEST_CASE("settings search finds a setting in both locales", "[settings][search]") {
    // rules/ui.md makes Thai a first-class locale, and the schema carries Thai
    // keywords precisely so a Thai speaker can FIND a setting by typing Thai. A
    // translated label with English-only search is half a locale, and it is the
    // half nobody notices is missing until someone tries.
    const krait::app::settings::Def* fontSize = krait::app::settings::find("font.size");
    REQUIRE(fontSize != nullptr);

    CHECK(krait::app::settings::matchesSearch(*fontSize, ""));      // empty matches all
    CHECK(krait::app::settings::matchesSearch(*fontSize, "font"));  // by id
    CHECK(krait::app::settings::matchesSearch(*fontSize, "FONT"));  // ASCII case-insensitive
    CHECK_FALSE(krait::app::settings::matchesSearch(*fontSize, "zzzznotasetting"));

    // Every setting must be findable by SOMETHING in each locale, or it is a
    // setting only reachable by scrolling — which for a growing schema is the
    // same as unreachable.
    for (const krait::app::settings::Def& def : krait::app::settings::definitions()) {
        CAPTURE(std::string(def.id));
        CHECK_FALSE(def.searchEn.empty());
        CHECK_FALSE(def.searchTh.empty());
        // The first Thai keyword, whatever it is, has to actually match.
        const std::string_view thai = def.searchTh.substr(0, def.searchTh.find(' '));
        CHECK(krait::app::settings::matchesSearch(def, thai));
    }
}
