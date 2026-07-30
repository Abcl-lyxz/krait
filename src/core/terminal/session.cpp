#include "core/terminal/session.h"

#include "core/parser/csi_cursor.h"
#include "core/parser/csi_mode.h"
#include "core/parser/csi_scroll.h"
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
        handleMode(m_grid, params, intermediates, final);
    } else if (final == 'c' || final == 'n') {
        std::string reply;
        handleReport(m_grid, m_caps, params, intermediates, final, m_limiter, reply);
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

void Session::oscStart() {}

void Session::oscPut(std::uint8_t) {}

void Session::oscEnd(bool) {}

}  // namespace krait::core::vt
