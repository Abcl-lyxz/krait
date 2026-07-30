#include "core/caps/caps.h"
#include "core/grid/grid.h"
#include "core/grid/sync_output.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using krait::core::vt::Capabilities;
using krait::core::vt::decrqmState;
using krait::core::vt::Grid;
using krait::core::vt::ModeReport;
using krait::core::vt::SyncOutput;

TEST_CASE("sync output: the guard releases a batch the client never closed", "[modes]") {
    // The whole point of mode 2026's timeout. An application that sets 2026
    // and then blocks, or dies, must not be able to freeze rendering.
    SyncOutput sync;
    CHECK_FALSE(sync.holding(0));

    sync.begin(1'000);
    CHECK(sync.holding(1'000));
    CHECK(sync.holding(1'000 + SyncOutput::kTimeoutMs - 1));

    // Exactly at the guard, frames resume.
    CHECK_FALSE(sync.holding(1'000 + SyncOutput::kTimeoutMs));
    CHECK(sync.expired(1'000 + SyncOutput::kTimeoutMs));

    // But the MODE is still what the application set — the guard is a
    // rendering safety net, not a mode change.
    CHECK(sync.requested());
}

TEST_CASE("sync output: a closed batch is not expired", "[modes]") {
    SyncOutput sync;
    sync.begin(500);
    sync.end();
    CHECK_FALSE(sync.holding(600));
    CHECK_FALSE(sync.expired(10'000));
    CHECK_FALSE(sync.requested());
}

TEST_CASE("sync output: reopening restarts the guard", "[modes]") {
    SyncOutput sync;
    sync.begin(0);
    REQUIRE_FALSE(sync.holding(SyncOutput::kTimeoutMs));
    sync.begin(SyncOutput::kTimeoutMs);
    CHECK(sync.holding(SyncOutput::kTimeoutMs));
}

TEST_CASE("decrqm: a mode we cannot turn off answers 3, never 1", "[modes]") {
    // The honesty rule with teeth: 1 promises an application it can CHANGE the
    // mode. For anything permanently on, that promise is a lie it will act on.
    Grid g(4, 8);
    const Capabilities caps;

    CHECK(decrqmState(g, caps, 2027) == ModeReport::PermanentlySet);
    CHECK(decrqmState(g, caps, 7) == ModeReport::PermanentlySet);
    CHECK(decrqmState(g, caps, 25) == ModeReport::PermanentlySet);
}

TEST_CASE("decrqm: unimplemented modes answer 0, not 2", "[modes]") {
    // 2 means "reset, you may set it". For a mode we would ignore, that is the
    // same lie as 1 — it just fails one step later.
    Grid g(4, 8);
    const Capabilities caps;

    // 1000 lived here until T27 implemented mouse tracking. 1005 (UTF-8 mouse)
    // and 1015 (urxvt mouse) took its place: both are real xterm modes we
    // deliberately do not implement, so they are the honest stand-ins.
    CHECK(decrqmState(g, caps, 1005) == ModeReport::NotRecognized);
    CHECK(decrqmState(g, caps, 1015) == ModeReport::NotRecognized);
    CHECK(decrqmState(g, caps, 9999) == ModeReport::NotRecognized);
    CHECK(decrqmState(g, caps, 0) == ModeReport::NotRecognized);
}

TEST_CASE("decrqm: the mouse tracking modes share one variable", "[modes]") {
    // 1000/1002/1003 are mutually exclusive in xterm. Three independent flags
    // would answer Set for two of them at once, and an application that
    // disabled one would keep receiving the other's reports as keyboard input.
    Grid g(4, 8);
    const Capabilities caps;

    CHECK(decrqmState(g, caps, 1000) == ModeReport::Reset);
    CHECK(decrqmState(g, caps, 1002) == ModeReport::Reset);
    CHECK(decrqmState(g, caps, 1003) == ModeReport::Reset);

    g.mouseTracking = Grid::MouseTracking::AnyEvent;
    CHECK(decrqmState(g, caps, 1003) == ModeReport::Set);
    CHECK(decrqmState(g, caps, 1000) == ModeReport::Reset);
    CHECK(decrqmState(g, caps, 1002) == ModeReport::Reset);

    // The encoding is independent of the tracking mode.
    CHECK(decrqmState(g, caps, 1006) == ModeReport::Reset);
    g.sgrMouse = true;
    CHECK(decrqmState(g, caps, 1006) == ModeReport::Set);

    // DECCKM is a plain toggle and must never answer 3.
    CHECK(decrqmState(g, caps, 1) == ModeReport::Reset);
    g.appCursorKeys = true;
    CHECK(decrqmState(g, caps, 1) == ModeReport::Set);
}

TEST_CASE("decrqm: toggleable modes track live state", "[modes]") {
    Grid g(4, 8);
    const Capabilities caps;

    CHECK(decrqmState(g, caps, 6) == ModeReport::Reset);
    g.originMode = true;
    CHECK(decrqmState(g, caps, 6) == ModeReport::Set);

    CHECK(decrqmState(g, caps, 1049) == ModeReport::Reset);
    g.useAlternateScreen(true);
    CHECK(decrqmState(g, caps, 1049) == ModeReport::Set);

    CHECK(decrqmState(g, caps, 2004) == ModeReport::Reset);
    g.bracketedPaste = true;
    CHECK(decrqmState(g, caps, 2004) == ModeReport::Set);
}

TEST_CASE("decrqm: 2026 reports the request, not the guard", "[modes]") {
    Grid g(4, 8);
    const Capabilities caps;

    CHECK(decrqmState(g, caps, 2026) == ModeReport::Reset);
    g.sync.begin(0);
    CHECK(decrqmState(g, caps, 2026) == ModeReport::Set);

    // Guard has long since fired; the application still owns the mode.
    REQUIRE(g.sync.expired(10'000));
    CHECK(decrqmState(g, caps, 2026) == ModeReport::Set);
}

TEST_CASE("decrqm: the capability table is the source, not a constant", "[modes]") {
    // Flip the table and the reply must follow — the two cannot disagree by
    // construction, which is the point of generating replies from it.
    Grid g(4, 8);
    Capabilities caps;
    caps.graphemeClusteringAlwaysOn = false;
    CHECK(decrqmState(g, caps, 2027) == ModeReport::NotRecognized);
}
