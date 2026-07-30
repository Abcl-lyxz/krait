#pragma once

#include "core/grid/grid.h"
#include "core/parser/events.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace krait::core::vt {

// The capability table (CLAUDE.md honesty rule): DA1 is GENERATED from
// these flags, and a flag may only be true when the feature is actually
// implemented and corpus-tested. M0 truth: SGR attributes exist (AVO);
// nothing VT220-level does yet.
struct Capabilities {
    bool avo = true;  // bold/underline/blink attributes (SGR basic, T7)
    bool columns132 = false;
    bool printerPort = false;
    bool regis = false;
    bool sixel = false;
    bool selectiveErase = false;
    bool ansiColor = false;  // claimed only via the VT220+ identity, M1
    // Mode 2027 (grapheme clustering). Our width model is ALWAYS cluster-based
    // — utf8proc segmentation, never per-codepoint wcwidth (T19, ADR-0003) —
    // so this is not a switch we own the "off" position of. DECRQM (T22) must
    // therefore answer 3 (PERMANENTLY SET) for 2027, never 1 (set), and
    // DECRST 2027 is accepted-and-ignored rather than obeyed. Reporting 1
    // would promise an application it can turn clustering off.
    //
    // Deliberately NOT a DA1 code: DA1 advertises VT-level hardware features,
    // and 2027 is negotiated through DECRQM alone.
    bool graphemeClusteringAlwaysOn = true;
};

// Builds the DA1 reply from the table. With no VT220-level feature on, the
// honest identity is VT100 with AVO (ESC [ ? 1 ; 2 c) — or, with avo off,
// "VT101 with no options" (ESC [ ? 1 ; 0 c).
std::string da1Reply(const Capabilities& caps);

// Reply flood guard: hostile input can request reports in a tight loop and
// use the terminal as an amplifier. Credits refill from INPUT volume, so
// the core stays clock-free and deterministic. Integrator contract: call
// addInput(chunk.size()) for every input chunk BEFORE feeding it to the
// parser — the corpus harness mirrors this wiring.
class ReplyLimiter {
  public:
    static constexpr int kRepliesPerWindow = 8;
    static constexpr std::size_t kWindowBytes = 256;

    void addInput(std::size_t bytes) noexcept {
        m_pending += bytes;
        while (m_pending >= kWindowBytes) {
            m_pending -= kWindowBytes;
            m_credits = kRepliesPerWindow;
        }
    }

    bool allow() noexcept {
        if (m_credits <= 0) {
            return false;
        }
        --m_credits;
        return true;
    }

  private:
    std::size_t m_pending = 0;
    int m_credits = kRepliesPerWindow;
};

// DECRQM's answer values (DEC STD 070 / ctlseqs "CSI ? Ps ; Pm $ y"). The
// distinction that matters is 1/2 versus 3/4: 1 and 2 promise an application
// it can CHANGE the mode, 3 and 4 tell it the answer is fixed. Reporting 1 for
// something we cannot actually turn off is the exact dishonesty CLAUDE.md's
// capability rule exists to prevent.
enum class ModeReport : std::uint8_t {
    NotRecognized = 0,
    Set = 1,
    Reset = 2,
    PermanentlySet = 3,
    PermanentlyReset = 4,
};

// The single source of truth for "what is mode Ps doing", generated from live
// grid state and the capability table — never a hardcoded answer. DECRQM and
// any future XTGETTCAP-style query both read it, so the two cannot disagree.
ModeReport decrqmState(const Grid& grid, const Capabilities& caps, std::uint16_t mode) noexcept;

// DA1 (CSI c) and DSR 5/6 (CSI n). Appends the reply to `out` subject to
// the limiter (a rate-dropped reply still counts as handled). Returns false
// for anything else — DA2, DECXCPR, DEC ?-forms and colon subparams are
// honest silence until implemented.
bool handleReport(const Grid& grid, const Capabilities& caps, const Params& params,
                  std::span<const std::uint8_t> intermediates, std::uint8_t final,
                  ReplyLimiter& limiter, std::string& out);

// DECRQM: `CSI ? Ps $ p` -> `CSI ? Ps ; Pm $ y`. Separate from handleReport
// because it is selected by an INTERMEDIATE ('$') rather than by its final
// byte, and folding it in would blur that dispatch.
bool handleDecrqm(const Grid& grid, const Capabilities& caps, const Params& params,
                  std::span<const std::uint8_t> intermediates, std::uint8_t final,
                  ReplyLimiter& limiter, std::string& out);

}  // namespace krait::core::vt
