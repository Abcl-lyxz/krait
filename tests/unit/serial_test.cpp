// T56: serial port identification.
//
// A serial backend is mostly Win32 calls against hardware that is not plugged
// into a CI runner, so what is tested here is the part with a DECISION in it:
// turning a Windows hardware id into a VID/PID, and deciding whether a port
// that just appeared is the same adapter that was unplugged.
//
// That second question is the whole feature. Windows reuses COM numbers, so
// after a replug the port we lost can be taken by a DIFFERENT adapter — and
// reopening that one would connect the session to a device nobody chose.

#include "serial/serial_ports.h"
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ks = krait::net::serial;

TEST_CASE("a USB hardware id yields its VID and PID", "[serial]") {
    std::string vendor;
    std::string product;
    // Microsoft's Standard USB Identifiers: four hex digits each.
    REQUIRE(ks::parseUsbIds("USB\\VID_10C4&PID_EA60", &vendor, &product));
    CHECK(vendor == "10C4");
    CHECK(product == "EA60");
}

TEST_CASE("a composite device's MI suffix is not swallowed", "[serial]") {
    // Most USB-serial adapters are composite, and their hardware ids carry an
    // interface number after the PID. Reading greedily past four digits would
    // make the product id "EA60" plus whatever followed, and no two
    // enumerations would agree.
    std::string vendor;
    std::string product;
    REQUIRE(ks::parseUsbIds("USB\\VID_0403&PID_6001&MI_00", &vendor, &product));
    CHECK(vendor == "0403");
    CHECK(product == "6001");
}

TEST_CASE("hardware ids are case-insensitive and normalised", "[serial]") {
    // Windows is inconsistent about case here, and a reconnect compares these
    // as strings — so "ea60" and "EA60" being different would break the replug
    // match on exactly the adapters that report lowercase.
    std::string vendor;
    std::string product;
    REQUIRE(ks::parseUsbIds("USB\\VID_10c4&PID_ea60", &vendor, &product));
    CHECK(vendor == "10C4");
    CHECK(product == "EA60");
}

TEST_CASE("a non-USB or malformed id is refused, not guessed", "[serial]") {
    std::string vendor;
    std::string product;
    // A built-in COM port: ACPI, no VID at all.
    CHECK_FALSE(ks::parseUsbIds("ACPI\\PNP0501", &vendor, &product));
    // Truncated: three digits is not a VID, and accepting it would produce an
    // identity that matches the wrong device later.
    CHECK_FALSE(ks::parseUsbIds("USB\\VID_10C&PID_EA60", &vendor, &product));
    CHECK_FALSE(ks::parseUsbIds("USB\\VID_10C4", &vendor, &product));
    CHECK_FALSE(ks::parseUsbIds("", &vendor, &product));
}

TEST_CASE("the label never comes out blank", "[serial]") {
    ks::PortInfo port;
    port.name = "COM7";
    CHECK(port.label() == "COM7");

    port.vendorId = "10C4";
    port.productId = "EA60";
    CHECK(port.label() == "COM7 (USB 10C4:EA60)");

    port.friendlyName = "Silicon Labs CP210x USB to UART Bridge (COM7)";
    CHECK(port.label() == port.friendlyName);
}

TEST_CASE("the same adapter is recognised on a different COM number", "[serial]") {
    // The replug case. Windows hands the adapter whatever number is free, so
    // matching on the name would miss the device that actually came back.
    ks::PortInfo before;
    before.name = "COM7";
    before.vendorId = "10C4";
    before.productId = "EA60";

    ks::PortInfo after = before;
    after.name = "COM12";
    CHECK(before.sameDevice(after));
}

TEST_CASE("a different adapter on the same number is NOT the same device", "[serial]") {
    // The bug this prevents: unplug the console cable, plug in a different
    // adapter, Windows gives it COM7, and the session silently reattaches to
    // hardware nobody asked for.
    ks::PortInfo before;
    before.name = "COM7";
    before.vendorId = "10C4";
    before.productId = "EA60";

    ks::PortInfo other;
    other.name = "COM7";
    other.vendorId = "0403";
    other.productId = "6001";
    CHECK_FALSE(before.sameDevice(other));
}

TEST_CASE("without hardware ids the port name is all there is", "[serial]") {
    // A built-in COM port has no VID/PID. Falling back to the name is the
    // honest limit of what Windows exposes without opening the device — worth
    // stating in a test so nobody later reads the fallback as a bug.
    ks::PortInfo builtin;
    builtin.name = "COM1";
    ks::PortInfo same;
    same.name = "COM1";
    ks::PortInfo different;
    different.name = "COM2";
    CHECK(builtin.sameDevice(same));
    CHECK_FALSE(builtin.sameDevice(different));
}

TEST_CASE("enumeration does not throw or hang without hardware", "[serial]") {
    // A CI runner has no serial ports, and an empty list is the correct answer
    // rather than an error. This is the only case here that touches Win32.
    const std::vector<ks::PortInfo> ports = ks::enumeratePorts();
    for (const ks::PortInfo& port : ports) {
        CHECK_FALSE(port.name.empty());
        CHECK_FALSE(port.label().empty());
    }
    SUCCEED("enumerated " << ports.size() << " port(s)");
}
