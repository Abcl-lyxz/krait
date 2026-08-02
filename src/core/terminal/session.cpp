#include "core/terminal/session.h"

#include "core/parser/csi_cursor.h"
#include "core/parser/csi_mode.h"
#include "core/parser/csi_scroll.h"
#include "core/parser/kitty_keys.h"
#include "core/parser/sgr.h"

#include <algorithm>
#include <optional>

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
    placement.anchor = m_grid.stableLineOfScreenRow(m_grid.row);
    placement.rowInLine = m_grid.rowOffsetInLine(m_grid.row);
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
    // what makes a run of sixels stack instead of overprint.
    advanceBelowImage(rows);
}

void Session::advanceBelowImage(int rows) {
    // BOUNDED BY THE SCREEN, and that bound is the point rather than a
    // tidiness. Kitty's `r=` is chosen by the SENDER and clamped only at
    // 10000, so `a=T,s=1,v=1,c=1,r=10000` — a one-pixel image and about forty
    // bytes — would otherwise buy ten thousand line feeds, each of which
    // pushes a line into scrollback. A megabyte of those is a quarter of a
    // billion line operations: the same throughput denial of service the OSC
    // 133 A+D flood turned out to be in M4, arriving by a different door.
    //
    // Clamping costs nothing real: an image taller than the screen cannot be
    // shown in full anyway, so scrolling further than the screen height
    // accomplishes nothing a user could see.
    const int steps = std::min(rows, m_grid.rows);
    // The ordinary line feed, so scrollback capture, damage and the scrolling
    // region all behave exactly as they do for text.
    for (int i = 0; i < steps; ++i) {
        handleControl(m_grid, 0x0A);
    }
    m_grid.col = 0;
}

void Session::writeSizedText(const OscAction& action) {
    // OSC 66's text goes through the ORDINARY print path, one decoded codepoint
    // at a time, with the scale carried in the pen. Everything that makes text
    // work — grapheme clustering, the width tables, wrap, damage, scrollback
    // capture — is in putChar, and a second writer here would be a second set
    // of answers to all of it that could disagree with the first.
    //
    // The scale rides in Attr's spare flag bits (cell.h), so a sized cell costs
    // nothing extra in scrollback and survives reflow like any other.
    const Attr saved = m_grid.pen;
    m_grid.pen.setScale(action.scale);
    m_grid.pen.setSizeWidth(action.widthCells);

    const int startCol = m_grid.col;
    // The SAME decoder the ground path uses. A lone continuation byte or a
    // truncated sequence yields U+FFFD rather than being dropped: the payload
    // is remote text, and silently losing a byte is how a Thai cluster comes
    // apart into something that still renders and is no longer the word.
    Utf8Decoder decoder;
    char32_t decoded[2] = {};
    for (const char ch : action.text) {
        const int count = decoder.feed(static_cast<std::uint8_t>(ch), decoded);
        for (int i = 0; i < count; ++i) {
            m_grid.putChar(decoded[i]);
        }
    }
    // Flush: a payload ending mid-sequence must still produce its replacement
    // character, or the cell count the cursor was advanced by would be a lie.
    char32_t trailing[1] = {};
    if (decoder.finish(trailing) == 1) {
        m_grid.putChar(trailing[0]);
    }

    // w != 0 means "render all of this in s*w cells", so the cursor advances by
    // exactly that regardless of what the text measured. Clamped to the row:
    // the protocol lets a sender ask for more cells than the line has, and
    // putChar's wrap has already moved us if the text itself ran out of room.
    if (action.widthCells > 0) {
        const int wanted = action.scale * action.widthCells;
        m_grid.col = std::min(startCol + wanted, m_grid.cols - 1);
    }
    m_grid.pen = saved;
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
    case Command::Action::Unsupported:
        // Both are already finished by the reply above. A QUERY must not
        // transmit, place or allocate — it exists so a sender can find out
        // whether the protocol is spoken at all — and an UNSUPPORTED action has
        // been declined out loud, so there is nothing left to do for either.
        // Shared branch on purpose: bugprone-branch-clone gates this build,
        // exactly as it does for DECAWM and DECTCEM in caps.cpp.
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
    placement.anchor = m_grid.stableLineOfScreenRow(m_grid.row);
    placement.rowInLine = m_grid.rowOffsetInLine(m_grid.row);
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
        advanceBelowImage(placement.rows);
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
    if (action.kind == OscAction::Kind::SizedText) {
        // Handled entirely in the core and NOT forwarded. Unlike a prompt mark,
        // where the app adds a notification the core cannot, there is nothing
        // above this layer that could contribute to drawing text on a grid.
        writeSizedText(action);
        return;
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
