// T32 — the locale gate.
//
// rules/ui.md: "English + Thai ship together — a string landing without both
// locales is incomplete work."
//
// WHAT THIS FILE CAN AND CANNOT DO. It reads the two .ts files and checks them
// against EACH OTHER: no unfinished/vanished markers, the same messages in the
// same order, placeholders surviving translation. That catches a half-finished
// translation pass, a hand-edit that desynchronised the files, and a %1 dropped
// on the way into Thai.
//
// It does NOT catch a string that is missing from BOTH files, because nothing
// here reads the C++ or QML sources — a new tr() simply is not present, the two
// files stay consistent, and all three cases below pass. This comment used to
// claim the opposite ("fails the moment someone adds a string and does not
// translate it"), and believing it is exactly how T52 came within one review of
// shipping fifteen untranslated strings.
//
// The check that DOES read the sources is the "Translations cover every string"
// step in .github/workflows/ci.yml: it re-runs lupdate and fails if the
// catalogues change. Only lupdate knows what the sources contain, so that check
// cannot live here. Keep both — they fail on different things.

#include <catch2/catch_test_macros.hpp>

#include <QDomDocument>
#include <QFile>

#include <vector>

namespace {

QDomDocument loadTs(const QString& name) {
    QFile file(QStringLiteral(KRAIT_I18N_DIR) + "/" + name);
    REQUIRE(file.open(QIODevice::ReadOnly));
    QDomDocument doc;
    REQUIRE(doc.setContent(&file));
    return doc;
}

struct Message {
    QString context;
    QString source;
    QString translation;
    QString type;  // "unfinished"/"vanished" when lupdate marked it
};

std::vector<Message> messages(const QDomDocument& doc) {
    std::vector<Message> out;
    const QDomNodeList contexts = doc.elementsByTagName("context");
    for (int c = 0; c < contexts.count(); ++c) {
        const QDomElement context = contexts.at(c).toElement();
        const QString name = context.firstChildElement("name").text();
        for (QDomElement message = context.firstChildElement("message"); !message.isNull();
             message = message.nextSiblingElement("message")) {
            const QDomElement translation = message.firstChildElement("translation");
            out.push_back(Message{
                .context = name,
                .source = message.firstChildElement("source").text(),
                .translation = translation.text(),
                .type = translation.attribute("type"),
            });
        }
    }
    return out;
}

}  // namespace

TEST_CASE("every string has a Thai translation", "[i18n]") {
    // Thai is a first-class locale, not an afterthought. An untranslated string
    // is not a cosmetic gap: a half-Thai settings page is harder to use than an
    // English one, because the user cannot tell which half to trust.
    const auto thai = messages(loadTs("krait_th.ts"));
    REQUIRE_FALSE(thai.empty());
    for (const Message& message : thai) {
        INFO(message.context.toStdString() << ": " << message.source.toStdString());
        CHECK(message.type.isEmpty());  // not "unfinished", not "vanished"
        CHECK_FALSE(message.translation.isEmpty());
    }
}

TEST_CASE("English and Thai cover the same strings", "[i18n]") {
    // Drift here means one locale silently has fewer strings than the other,
    // which is exactly the state "ship together" exists to prevent.
    const auto english = messages(loadTs("krait_en.ts"));
    const auto thai = messages(loadTs("krait_th.ts"));
    CHECK(english.size() == thai.size());

    for (std::size_t i = 0; i < english.size() && i < thai.size(); ++i) {
        INFO("index " << i);
        CHECK(english[i].context == thai[i].context);
        CHECK(english[i].source == thai[i].source);
    }
}

TEST_CASE("a translation keeps the placeholders its source has", "[i18n]") {
    // A dropped %1 is not a typo: the message silently loses the exit code, the
    // host name or the path it was meant to carry — and only in the locale
    // nobody on the team reads.
    for (const QString& file : {QStringLiteral("krait_en.ts"), QStringLiteral("krait_th.ts")}) {
        for (const Message& message : messages(loadTs(file))) {
            for (int n = 1; n <= 9; ++n) {
                const QString placeholder = QStringLiteral("%%%1").arg(n);
                if (!message.source.contains(placeholder)) {
                    continue;
                }
                INFO(file.toStdString() << " / " << message.source.toStdString());
                CHECK(message.translation.contains(placeholder));
            }
        }
    }
}
