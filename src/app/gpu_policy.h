#pragma once

#include <string_view>

namespace krait::app {

// Which adapter to ask Qt for (plan T26). rules/render.md requires a
// WARP/software fallback "for RDP and VMs": a hardware D3D11 device inside an
// RDP session is emulated anyway, badly, and on some hosts fails to create.
//
// Pure on purpose — the decision is the part worth testing, and asserting it
// must not need a window, a GPU or an actual RDP session.
//
// `mode` is the KRAIT_GPU override, lowercased:
//   "warp" / "software" — always software, even on a local GPU
//   "hardware"          — never software, even over RDP (the escape hatch for a
//                         host where the remote-session probe lies)
//   anything else       — auto: software only when the session is remote
inline bool preferSoftwareDevice(std::string_view mode, bool remoteSession) {
    if (mode == "hardware") {
        return false;
    }
    return mode == "warp" || mode == "software" || remoteSession;
}

}  // namespace krait::app
