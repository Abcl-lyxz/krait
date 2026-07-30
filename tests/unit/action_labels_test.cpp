#include "actions.h"
#include "session_model.h"
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>

using krait::app::translatableActionLabels;
using krait::app::session::Action;
using krait::app::session::allActions;

TEST_CASE("every action label is visible to lupdate", "[session][i18n]") {
    // The palette translates with tr(entry.label.c_str()) because the action
    // registry is deliberately Qt-free, and lupdate cannot see through a runtime
    // string. session_model.cpp therefore repeats each label as a QT_TR_NOOP
    // literal so the extractor finds it.
    //
    // That duplication is exactly the kind of thing that rots silently — M1 lost
    // eight strings to the same shape of mistake, a translate() lambda hiding
    // literals behind one level of indirection. So the two lists are compared
    // rather than trusted: rename an action and this fails, which is the point.
    std::set<std::string_view> declared;
    for (const Action& action : allActions()) {
        declared.insert(action.label);
    }

    std::set<std::string_view> extractable;
    for (const char* const label : translatableActionLabels()) {
        extractable.insert(label);
    }

    for (const std::string_view label : declared) {
        CAPTURE(std::string(label));
        // Missing here means this action ships untranslated, in every locale,
        // with nothing anywhere to say that it did.
        CHECK(extractable.contains(label));
    }
    for (const std::string_view label : extractable) {
        CAPTURE(std::string(label));
        // Present here but gone from the registry means a stale entry the
        // translators are still being asked to translate.
        CHECK(declared.contains(label));
    }
    CHECK(declared.size() == extractable.size());
}
