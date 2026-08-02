#include "taskbar_progress.h"
#include <catch2/catch_test_macros.hpp>

#include <vector>

using krait::app::aggregateProgress;
using krait::app::Progress;
using krait::app::TabProgress;
using krait::app::TaskbarState;

namespace {

TaskbarState aggregate(const std::vector<TabProgress>& tabs) {
    return aggregateProgress(tabs);
}

}  // namespace

// The COM half of taskbar_progress.cpp is untestable without a taskbar and a
// window; the DECISION half is this function, which is why it is a free
// function. What the tests below pin is the answer to "two tabs disagree —
// what does the one button show".

TEST_CASE("taskbar: no tab reporting means no bar", "[app][taskbar]") {
    CHECK(aggregate({}) == TaskbarState{});
    // A tab that explicitly removed its progress votes for nothing, which is
    // not the same as it voting for zero.
    CHECK(aggregate({{.state = Progress::Remove, .percent = -1},
                     {.state = Progress::Remove, .percent = 90}}) == TaskbarState{});
}

TEST_CASE("taskbar: one tab passes straight through", "[app][taskbar]") {
    const TaskbarState one = aggregate({{.state = Progress::Set, .percent = 42}});
    CHECK(one.state == Progress::Set);
    CHECK(one.percent == 42);
}

TEST_CASE("taskbar: severity beats a percentage", "[app][taskbar]") {
    // THE tie-break, and the reason it goes this way: the bar exists to be read
    // from behind another window. A tab whose build failed must not be outvoted
    // by another that happens to be 60% through a download — a percentage is
    // still on the tab itself, an error on a hidden tab is not.
    const TaskbarState mixed = aggregate(
        {{.state = Progress::Set, .percent = 60}, {.state = Progress::Error, .percent = 10}});
    CHECK(mixed.state == Progress::Error);
    CHECK(mixed.percent == 10);

    // Full order: Error > Paused > Indeterminate > Set > Remove. NOT the enum's
    // numeric order, which is the wire order — OSC 9;4 numbers indeterminate 3
    // and paused 4, and taking that at face value would rank paused over error.
    CHECK(aggregate(
              {{.state = Progress::Paused, .percent = 5}, {.state = Progress::Error, .percent = 5}})
              .state == Progress::Error);
    CHECK(aggregate({{.state = Progress::Indeterminate, .percent = -1},
                     {.state = Progress::Paused, .percent = 5}})
              .state == Progress::Paused);
    CHECK(aggregate({{.state = Progress::Set, .percent = 99},
                     {.state = Progress::Indeterminate, .percent = -1}})
              .state == Progress::Indeterminate);
}

TEST_CASE("taskbar: equal severity takes the least-finished tab", "[app][taskbar]") {
    // The button reaches 100% only when every reporting tab has. Taking the
    // maximum would show a finished bar while work was still running, which is
    // the one reading a user actually acts on.
    const TaskbarState both = aggregate({{.state = Progress::Set, .percent = 90},
                                         {.state = Progress::Set, .percent = 20},
                                         {.state = Progress::Set, .percent = 55}});
    CHECK(both.state == Progress::Set);
    CHECK(both.percent == 20);
}

TEST_CASE("taskbar: an omitted percentage reads by state", "[app][taskbar]") {
    // OSC 9;4 states 2 and 4 may omit the percentage (ConEmu says so). A full
    // red bar is what a failure looks like; an empty one is indistinguishable
    // from no progress at all.
    CHECK(aggregate({{.state = Progress::Error, .percent = -1}}).percent == 100);
    CHECK(aggregate({{.state = Progress::Paused, .percent = -1}}).percent == 100);
    // Set with no percentage has told us nothing, so 0 is the honest bar.
    CHECK(aggregate({{.state = Progress::Set, .percent = -1}}).percent == 0);
    // ...and it still loses the min against a tab that did supply one.
    CHECK(aggregate({{.state = Progress::Error, .percent = -1},
                     {.state = Progress::Error, .percent = 30}})
              .percent == 30);
}

TEST_CASE("taskbar: a state with no bar pins its percentage", "[app][taskbar]") {
    // Remove and Indeterminate draw no fillable bar, so the percentage is
    // meaningless there — pinned to 0 so two equal states compare equal and the
    // throttle can skip the COM call rather than poking the taskbar with a
    // value nothing will show.
    CHECK(aggregate({{.state = Progress::Indeterminate, .percent = 77}}) ==
          TaskbarState{.state = Progress::Indeterminate, .percent = 0});
}
