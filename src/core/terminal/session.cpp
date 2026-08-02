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

void Session::dcsHook(const Params& params, std::span<const std::uint8_t> intermediates,
                      std::uint8_t final) {
    // Sixel is DCS P1;P2;P3 q with NO intermediates (T79). Everything else —
    // DECRQSS (a '$' intermediate before 'q'), DECUDK ('|'), XTGETTCAP ('+q') —
    // falls through to honest silence, and the intermediates check is what
    // keeps them there: 'q' preceded by '$' is DECRQSS asking about a setting,
    // and reading it as a picture would swallow a query somebody is waiting on.
    m_inSixel = final == 'q' && intermediates.empty();
    if (m_inSixel) {
        m_sixel.begin(params);
    }
}

void Session::dcsPut(std::uint8_t byte) {
    if (m_inSixel) {
        m_sixel.put(byte);
    }
}

void Session::dcsUnhook(bool aborted) {
    if (!m_inSixel) {
        return;
    }
    m_inSixel = false;
    std::optional<Image> image = m_sixel.end(aborted);
    if (!image) {
        return;
    }

    // Cell size comes from the renderer (grid.h). Without it there is no honest
    // way to say how many cells the picture covers, so the image is dropped
    // rather than placed at a guessed size — a picture in the wrong place is
    // worse than one that never appeared, and the only way to be here without a
    // cell size is a headless test or a bench run.
    if (m_grid.cellWidthPx <= 0 || m_grid.cellHeightPx <= 0) {
        return;
    }
    const int cols = (image->width + m_grid.cellWidthPx - 1) / m_grid.cellWidthPx;
    const int rows = (image->height + m_grid.cellHeightPx - 1) / m_grid.cellHeightPx;
    const int width = image->width;
    const int height = image->height;

    const std::uint32_t id = m_grid.images.put(0, std::move(*image));
    if (id == 0) {
        return;  // refused by the byte budget; nothing to place
    }

    Placement placement;
    placement.imageId = id;
    // Anchored to the LINE the cursor is on, in scrollback's stable space, so
    // the picture rides scrolling, eviction and reflow the way an OSC 133 mark
    // does. A screen row would slide onto different text the moment the window
    // was dragged — the landmine CLAUDE.md records for scrollback.
    placement.anchor =
        m_grid.scrollback().linesEverStarted() + static_cast<std::uint64_t>(m_grid.row);
    placement.col = m_grid.col;
    placement.cols = cols;
    placement.rows = rows;
    placement.srcW = width;
    placement.srcH = height;
    if (!m_grid.images.place(placement)) {
        m_grid.images.erase(id);
        return;
    }

    // DEC leaves the cursor at the start of the line BELOW the image, which is
    // what makes a run of sixels stack instead of overprint. Done with the
    // ordinary line feed so scrollback capture, damage and the scrolling region
    // all behave exactly as they do for text.
    for (int i = 0; i < rows; ++i) {
        handleControl(m_grid, 0x0A);
    }
    m_grid.col = 0;
}

void Session::apcStart() {
    m_kitty.start();
}

void Session::apcPut(std::uint8_t byte) {
    m_kitty.put(byte);
}

void Session::apcEnd(bool aborted) {
    const std::optional<Command> command = m_kitty.end(aborted);
    if (!command) {
        return;  // not a graphics command, aborted, or a chunk with more to come
    }

    // The reply goes out FIRST, before anything is drawn. A sender that asked
    // for confirmation is usually waiting on it before sending the next image,
    // and a terminal that answered only after placing would serialise a
    // transfer behind its own rendering.
    if (const std::string reply = kittyReply(*command); !reply.empty() && onReply) {
        onReply(reply);
    }
    if (!command->error.empty()) {
        return;  // declined; the sender has been told and can fall back
    }

    switch (command->action) {
    case Command::Action::Delete:
        if (command->id != 0) {
            m_grid.images.erase(command->id);
        } else {
            // A bare `a=d` deletes every placement but KEEPS the images: that
            // is kitty's `d=a` default, and dropping the pixels too would make
            // a later `a=p` fail for an image the sender never withdrew.
            m_grid.images.clear();
        }
        return;
    case Command::Action::Query:
        // Answered by the reply above and nothing else. A query must not
        // transmit, place or allocate — it exists so a sender can find out
        // whether the protocol is spoken at all.
        return;
    case Command::Action::Unsupported:
        return;
    case Command::Action::Transmit:
    case Command::Action::TransmitAndPut:
    case Command::Action::Put:
        break;
    }

    std::uint32_t id = command->id;
    if (command->action != Command::Action::Put) {
        if (command->image.empty()) {
            return;
        }
        Image image = command->image;
        id = m_grid.images.put(command->id, std::move(image));
        if (id == 0) {
            return;  // refused by the byte budget
        }
    }
    if (command->action == Command::Action::Transmit) {
        return;  // stored, not shown: the sender will place it later
    }

    if (m_grid.cellWidthPx <= 0 || m_grid.cellHeightPx <= 0) {
        return;  // no cell size; see dcsUnhook for why this refuses to guess
    }
    const Image* stored = m_grid.images.find(id);
    if (stored == nullptr) {
        return;  // a=p naming an image that is not here
    }

    Placement placement;
    placement.imageId = id;
    placement.anchor =
        m_grid.scrollback().linesEverStarted() + static_cast<std::uint64_t>(m_grid.row);
    placement.col = m_grid.col;
    placement.zIndex = command->zIndex;
    // c= and r= are the sender's chosen size in CELLS and win when given; the
    // fallback is the image's own pixel size divided by the cell size.
    const int srcW = command->srcW > 0 ? command->srcW : stored->width;
    const int srcH = command->srcH > 0 ? command->srcH : stored->height;
    placement.srcX = command->srcX;
    placement.srcY = command->srcY;
    placement.srcW = srcW;
    placement.srcH = srcH;
    placement.cols =
        command->cols > 0 ? command->cols : (srcW + m_grid.cellWidthPx - 1) / m_grid.cellWidthPx;
    placement.rows =
        command->rows > 0 ? command->rows : (srcH + m_grid.cellHeightPx - 1) / m_grid.cellHeightPx;
    if (!m_grid.images.place(placement)) {
        return;
    }

    // C=1 asks the cursor to stay put, which is what a program drawing a
    // background or overlaying several images at chosen positions needs.
    // Without it the cursor lands below the image, as it does after a sixel.
    if (!command->cursorStays) {
        for (int i = 0; i < placement.rows; ++i) {
            handleControl(m_grid, 0x0A);
        }
        m_grid.col = 0;
    }
}

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
