#pragma once

#include <cstdint>

namespace krait::core::vt {

// Mode 2026, synchronized output. An application brackets a frame with
// DECSET 2026 / DECRST 2026 so the renderer presents the finished result
// instead of a half-drawn one.
//
// The timeout is not optional politeness, it is the safety net: an application
// that sets 2026 and then blocks — or dies — would otherwise freeze the
// terminal forever, and rules/vt-core.md requires a ~150 ms guard so a stuck
// client can never do that.
//
// Time is PASSED IN rather than read. src/core/ owns no clock (the same reason
// ReplyLimiter meters on input volume instead of seconds): a clock here would
// make every test time-dependent and drag a platform dependency into the
// sacred zone. The app layer supplies a monotonic millisecond stamp.
class SyncOutput {
  public:
    // ~150 ms. Long enough for any real frame, short enough that a user reads
    // a stuck client as a slow redraw rather than a hang.
    static constexpr std::uint64_t kTimeoutMs = 150;

    void begin(std::uint64_t nowMs) {
        m_requested = true;
        m_startMs = nowMs;
    }

    void end() { m_requested = false; }

    // Whether the renderer should still be holding frames back. False once the
    // guard has expired, even though the application has not ended its batch.
    bool holding(std::uint64_t nowMs) const {
        return m_requested && (nowMs - m_startMs) < kTimeoutMs;
    }

    // What the APPLICATION asked for, ignoring the guard. DECRQM reports this:
    // the timeout is a rendering safety net, not a mode change, and telling an
    // application its mode spontaneously reset would be a lie about state it
    // owns.
    bool requested() const { return m_requested; }

    // True when the guard fired on a batch the application never closed —
    // the renderer resumed on its own. Worth surfacing in diagnostics.
    bool expired(std::uint64_t nowMs) const { return m_requested && !holding(nowMs); }

  private:
    bool m_requested = false;
    std::uint64_t m_startMs = 0;
};

}  // namespace krait::core::vt
