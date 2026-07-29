#include "core/caps/caps.h"

#include <format>

namespace krait::core::vt {

std::string da1Reply(const Capabilities& caps) {
    // VT220-level identity only when a VT220-level feature is truthfully on.
    std::string ext;
    const auto add = [&ext](bool on, int code) {
        if (on) {
            ext += ';';
            ext += std::to_string(code);
        }
    };
    add(caps.columns132, 1);
    add(caps.printerPort, 2);
    add(caps.regis, 3);
    add(caps.sixel, 4);
    add(caps.selectiveErase, 6);
    add(caps.ansiColor, 22);
    if (ext.empty()) {
        return caps.avo ? "\x1B[?1;2c" : "\x1B[?1;0c";
    }
    return "\x1B[?62" + ext + "c";
}

bool handleReport(const Grid& grid, const Capabilities& caps, const Params& params,
                  std::span<const std::uint8_t> intermediates, std::uint8_t final,
                  ReplyLimiter& limiter, std::string& out) {
    if (!intermediates.empty()) {
        return false;  // DA2 (>), DECXCPR/DEC DSR (?), etc.: not implemented
    }
    for (std::size_t i = 0; i < params.count; ++i) {
        if (params.subparam[i]) {
            return false;  // colon subparams are SGR-only
        }
    }
    switch (final) {
    case 'c':  // DA1: only CSI c / CSI 0 c is ours (xterm ignores 0;1 forms)
        if (params.count > 1 || (params.count == 1 && params.values[0] != 0)) {
            return false;
        }
        if (limiter.allow()) {
            out += da1Reply(caps);
        }
        return true;
    case 'n': {  // DSR
        const int ps = params.count > 0 ? params.values[0] : 0;
        if (ps == 5) {  // operating status: OK
            if (limiter.allow()) {
                out += "\x1B[0n";
            }
            return true;
        }
        if (ps == 6) {  // CPR, 1-based (DECOM not implemented)
            if (limiter.allow()) {
                out += std::format("\x1B[{};{}R", grid.row + 1, grid.col + 1);
            }
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

}  // namespace krait::core::vt
