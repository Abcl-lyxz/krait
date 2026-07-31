#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace krait::net::serial {

// One serial port as the system describes it (plan T56).
//
// The friendly name and the VID/PID are what make this usable: "COM7" tells
// nobody which of the three adapters on the desk it is, and after a replug it
// may not even be the same one. The hardware id is also how a reconnect knows
// it found the SAME adapter rather than whichever device happened to take the
// name.
struct PortInfo {
    // "COM7". From the device's hardware key, value PortName — which Microsoft
    // documents as "typically COM<n>" and explicitly NOT guaranteed to be, so
    // callers must not parse a number out of it.
    std::string name;
    // "Silicon Labs CP210x USB to UART Bridge (COM7)", or empty. Routinely
    // absent, which is a normal path rather than a failure.
    std::string friendlyName;
    // Four hex digits each, uppercase, empty for a non-USB port. Taken from the
    // first hardware id that carries them.
    std::string vendorId;
    std::string productId;

    // What the picker shows. Falls back through friendly name, then hardware
    // ids, then the bare port name, so a row is never blank.
    std::string label() const;

    // Whether this is plausibly the same physical adapter as `other`. VID/PID
    // when both have them, otherwise the port name — two adapters of the same
    // model are indistinguishable this way, which is the honest limit of what
    // Windows exposes without opening each one.
    bool sameDevice(const PortInfo& other) const;
};

// Every serial port present now, sorted by name. Empty when there are none —
// which is the common case on a laptop and not an error.
std::vector<PortInfo> enumeratePorts();

// Parses "USB\VID_10C4&PID_EA60&MI_00" into its two halves. Exposed because it
// is the only part of enumeration with a decision in it, and the only part
// testable without a device plugged in.
//
// Windows hardware ids are a REG_MULTI_SZ list and a composite device's entries
// carry an &MI_zz suffix, so the parse must tolerate both more entries and more
// fields than it needs.
bool parseUsbIds(std::string_view hardwareId, std::string* vendorId, std::string* productId);

}  // namespace krait::net::serial
