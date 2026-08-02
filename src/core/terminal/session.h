#pragma once

#include "core/caps/caps.h"
#include "core/graphics/kitty.h"
#include "core/graphics/sixel.h"
#include "core/grid/grid.h"
#include "core/parser/events.h"
#include "core/parser/machine.h"
#include "core/parser/osc.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace krait::core::vt {

// One terminal session: parser + grid + the M0 handler set, behind a single
// feed() seam. Pure core — the app/backends deliver bytes in and take reply
// bytes out via onReply. OSC/DCS payloads are currently dropped (honestly:
// nothing claims them; see docs/conformance.md).
class Session final : public ParserEvents {
  public:
    Session(int rows, int cols);

    // Backend output bytes (hostile). Handles reply rate-limit accounting.
    void feed(std::span<const std::uint8_t> bytes);

    Grid& grid() { return m_grid; }

    const Grid& grid() const { return m_grid; }

    // Terminal -> application replies (DA1/DSR). Called during feed().
    std::function<void(const std::string&)> onReply;

    // An OSC string asked for something only the app layer can do: touch the
    // clipboard, open a link, retitle a window. The core decides WHAT was
    // asked and never does any of it — a VT core that reached a clipboard
    // would not be a VT core with zero platform deps (CLAUDE.md).
    std::function<void(const OscAction&)> onOsc;

    // OSC 52 READ permission, per session, off by default (rules/net.md).
    //
    // This is the sharpest thing in the whole protocol surface: with it on, any
    // program on the remote host can read the local clipboard — which may hold
    // a password just copied for a DIFFERENT machine. Write needs no permission
    // because it only ever costs the user a paste they did not expect; read
    // costs them a secret they never sent.
    void allowClipboardRead(bool allow) noexcept { m_clipboardReadAllowed = allow; }

    bool clipboardReadAllowed() const noexcept { return m_clipboardReadAllowed; }

    // ParserEvents
    void print(char32_t cp) override;
    void execute(std::uint8_t control) override;
    void escDispatch(std::span<const std::uint8_t> intermediates, std::uint8_t final) override;
    void csiDispatch(const Params& params, std::span<const std::uint8_t> intermediates,
                     std::uint8_t final) override;
    void dcsHook(const Params& params, std::span<const std::uint8_t> intermediates,
                 std::uint8_t final) override;
    void dcsPut(std::uint8_t byte) override;
    void dcsUnhook(bool aborted) override;
    void oscStart() override;
    void oscPut(std::uint8_t byte) override;
    void oscEnd(bool aborted) override;
    void apcStart() override;
    void apcPut(std::uint8_t byte) override;
    void apcEnd(bool aborted) override;

  private:
    // T81. OSC 66 text, written through the ordinary print path with the
    // scale carried in the pen. Private because it is not an event — the
    // parser never calls it; oscEnd does, for one Kind.
    void writeSizedText(const OscAction& action);
    // Moves the cursor below a just-placed image, BOUNDED BY THE SCREEN. The
    // bound is a denial-of-service fix, not tidiness — see the comment on the
    // definition. Shared by sixel and kitty so the two cannot diverge on it.
    void advanceBelowImage(int rows);

    Grid m_grid;
    Parser m_parser;
    // T79. One decoder reused across images rather than one per DCS: begin()
    // resets it completely, and a fresh decoder per sixel would reallocate the
    // 256-entry palette for every frame of an animation.
    SixelDecoder m_sixel;
    // Whether the DCS currently open is a sixel. Without this, dcsPut would
    // feed a DECRQSS query's bytes to the image decoder.
    bool m_inSixel = false;
    // T80. Kitty graphics, over APC. Held across escapes because a large image
    // arrives in chunks and only the first one carries the parameters.
    KittyDecoder m_kitty;
    Capabilities m_caps;
    ReplyLimiter m_limiter;
    OscHandler m_osc;
    bool m_clipboardReadAllowed = false;
};

}  // namespace krait::core::vt
