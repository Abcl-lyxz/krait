#include "core/terminal/session.h"

#include "core/parser/csi_cursor.h"
#include "core/parser/csi_mode.h"
#include "core/parser/csi_scroll.h"
#include "core/parser/kitty_keys.h"
#include "core/parser/sgr.h"

namespace krait::core::vt {

Session::Session(int rows, int cols) : m_grid(rows, cols), m_parser(*this) {}

void Session::feed(std::span<const std::uint8_t> bytes) {
    m_limiter.addInput(bytes.size());
    m_parser.feed(bytes);
}

void Session::print(char32_t cp) {
    m_grid.putChar(cp);
}

void Session::execute(std::uint8_t control) {
    handleControl(m_grid, control);
}

void Session::escDispatch(std::span<const std::uint8_t>, std::uint8_t) {
    // No ESC finals implemented yet (honest: nothing claimed).
}

void Session::csiDispatch(const Params& params, std::span<const std::uint8_t> intermediates,
                          std::uint8_t final) {
    if (final == 'm') {
        applySgr(m_grid.pen, params, intermediates);
    } else if (final == 'J' || final == 'K') {
        handleErase(m_grid, params, intermediates, final);
    } else if (final == 'r' || final == 'L' || final == 'M' || final == 'S' || final == 'T') {
        handleScroll(m_grid, params, intermediates, final);
    } else if (final == 'h' || final == 'l') {
        // Mode 2048 answers with the current size the moment it is enabled, and
        // it is the ONLY mode here that answers at all. Not rate-limited: it is
        // one reply per DECSET, the application asked for it by name, and a
        // dropped one leaves that application believing a size nobody told it —
        // which is the exact failure the mode exists to end.
        std::string reply;
        handleMode(m_grid, params, intermediates, final, &reply);
        if (!reply.empty() && onReply) {
            onReply(reply);
        }
    } else if (final == 'c' || final == 'n') {
        std::string reply;
        handleReport(m_grid, m_caps, params, intermediates, final, m_limiter, reply);
        if (!reply.empty() && onReply) {
            onReply(reply);
        }
    } else if (final == 'u') {
        // Kitty keyboard (T48). Selected by its private marker (> < = ?), so a
        // bare CSI u — which belongs to no protocol we speak — falls through to
        // the cursor handler and out the other side as silence.
        std::string reply;
        if (handleKittyKeys(m_grid, params, intermediates, final, m_limiter, reply)) {
            if (!reply.empty() && onReply) {
                onReply(reply);
            }
        }
    } else if (final == 'p') {
        // DECRQM is selected by its '?' '$' intermediates, not by 'p' alone —
        // handleDecrqm rejects every other 'p' form (DECSCL, DECSTR) so they
        // stay honest silence rather than answering as a mode query.
        std::string reply;
        handleDecrqm(m_grid, m_caps, params, intermediates, final, m_limiter, reply);
        if (!reply.empty() && onReply) {
            onReply(reply);
        }
    } else {
        handleCsiCursor(m_grid, params, intermediates, final);
    }
}

void Session::dcsHook(const Params&, std::span<const std::uint8_t>, std::uint8_t) {}

void Session::dcsPut(std::uint8_t) {}

void Session::dcsUnhook(bool) {}

void Session::oscStart() {
    m_osc.start();
}

void Session::oscPut(std::uint8_t byte) {
    m_osc.put(byte);
}

void Session::oscEnd(bool aborted) {
    const OscAction action = m_osc.end(aborted);
    if (action.kind == OscAction::Kind::None) {
        return;
    }
    if (action.kind == OscAction::Kind::PromptMark) {
        // Applied to the grid FIRST — the mark belongs on the grid line
        // (line.h) and nothing above the core places it. Then forwarded like
        // any other action: the C -> D transition is the only thing that tells
        // the app a command ran and for how long, and a wall clock is exactly
        // what src/core/ is not allowed to read (rules/vt-core.md). Nothing is
        // ever sent BACK either way — a prompt mark is an assertion, not a
        // query.
        m_grid.markPrompt(action.promptMark);
        if (action.promptMark == kMarkCommandEnd) {
            m_grid.setCommandExit(action.exitCode);
        }
    }
    if (action.kind == OscAction::Kind::ClipboardRead && !m_clipboardReadAllowed) {
        // Silence, not a refusal reply. Answering "no" still tells a remote
        // program that a clipboard exists and that asking is a thing it can do,
        // and there is nothing useful it could do with that except ask again.
        return;
    }
    if (onOsc) {
        onOsc(action);
    }
}

}  // namespace krait::core::vt
