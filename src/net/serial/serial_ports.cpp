#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "serial_ports.h"

#include <windows.h>
// After windows.h, and in this order: setupapi.h needs it, and ntddser.h is
// where GUID_DEVINTERFACE_COMPORT actually lives — not setupapi.h, which is
// the guess that costs an afternoon. initguid.h before it is what turns the
// GUID from a declaration into a definition in this translation unit.
#include <initguid.h>
#include <ntddser.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <vector>

namespace krait::net::serial {
namespace {

// Windows device property strings are comfortably under this; the buffer is
// grown on demand anyway, so it is a starting size rather than a limit.
constexpr DWORD kPropertyBytes = 1024;

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                          nullptr, nullptr);
    return out;
}

// Reads a device registry property. Returns the raw bytes, because
// SPDRP_HARDWAREID is REG_MULTI_SZ and the caller has to walk it.
std::vector<BYTE> deviceProperty(HDEVINFO set, SP_DEVINFO_DATA* device, DWORD property) {
    std::vector<BYTE> buffer(kPropertyBytes);
    DWORD required = 0;
    if (::SetupDiGetDeviceRegistryPropertyW(set, device, property, nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()),
                                            &required) != FALSE) {
        buffer.resize(required);
        return buffer;
    }
    // ERROR_INVALID_DATA means the property simply does not exist for this
    // device, which is routine for SPDRP_FRIENDLYNAME and not a failure.
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        return {};
    }
    buffer.resize(required);
    if (::SetupDiGetDeviceRegistryPropertyW(set, device, property, nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()),
                                            &required) == FALSE) {
        return {};
    }
    buffer.resize(required);
    return buffer;
}

std::wstring firstString(const std::vector<BYTE>& bytes) {
    if (bytes.size() < sizeof(wchar_t)) {
        return {};
    }
    const auto* text = reinterpret_cast<const wchar_t*>(bytes.data());
    const std::size_t chars = bytes.size() / sizeof(wchar_t);
    const std::size_t length = ::wcsnlen(text, chars);
    return {text, length};
}

// Walks a REG_MULTI_SZ. SPDRP_HARDWAREID is a LIST, and on a composite device
// (which most USB-serial adapters are) the entry carrying a usable VID/PID is
// not necessarily the first.
std::vector<std::wstring> multiString(const std::vector<BYTE>& bytes) {
    std::vector<std::wstring> out;
    if (bytes.size() < sizeof(wchar_t)) {
        return out;
    }
    const auto* text = reinterpret_cast<const wchar_t*>(bytes.data());
    std::size_t remaining = bytes.size() / sizeof(wchar_t);
    while (remaining > 0 && *text != L'\0') {
        const std::size_t length = ::wcsnlen(text, remaining);
        out.emplace_back(text, length);
        if (length >= remaining) {
            break;
        }
        text += length + 1;
        remaining -= length + 1;
    }
    return out;
}

// The "COMx" name, from the device's hardware key.
std::string portName(HDEVINFO set, SP_DEVINFO_DATA* device) {
    // DIREG_DEV is the hardware key. Failure is INVALID_HANDLE_VALUE, NOT
    // null — testing for null here would treat every failure as success.
    const HKEY key = ::SetupDiOpenDevRegKey(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::array<wchar_t, 64> value{};
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    DWORD type = 0;
    const LSTATUS status = ::RegQueryValueExW(key, L"PortName", nullptr, &type,
                                              reinterpret_cast<LPBYTE>(value.data()), &bytes);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        return {};
    }
    const std::size_t chars = bytes / sizeof(wchar_t);
    return narrow({value.data(), ::wcsnlen(value.data(), chars)});
}

}  // namespace

bool parseUsbIds(std::string_view hardwareId, std::string* vendorId, std::string* productId) {
    // "USB\VID_10C4&PID_EA60&MI_00" — four hex digits each per Microsoft's
    // Standard USB Identifiers, and the &MI_zz suffix a composite device adds
    // must not be swallowed into the product id.
    const auto field = [hardwareId](std::string_view key) -> std::string {
        const std::size_t at = hardwareId.find(key);
        if (at == std::string_view::npos) {
            return {};
        }
        const std::size_t start = at + key.size();
        std::string out;
        for (std::size_t i = start; i < hardwareId.size() && out.size() < 4; ++i) {
            const char ch = hardwareId[i];
            const bool hex =
                (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
            if (!hex) {
                break;
            }
            out += static_cast<char>(ch >= 'a' && ch <= 'f' ? ch - 'a' + 'A' : ch);
        }
        return out.size() == 4 ? out : std::string{};
    };

    const std::string vendor = field("VID_");
    const std::string product = field("PID_");
    if (vendor.empty() || product.empty()) {
        return false;
    }
    *vendorId = vendor;
    *productId = product;
    return true;
}

std::string PortInfo::label() const {
    if (!friendlyName.empty()) {
        return friendlyName;
    }
    if (!vendorId.empty()) {
        return name + " (USB " + vendorId + ":" + productId + ")";
    }
    return name;
}

bool PortInfo::sameDevice(const PortInfo& other) const {
    if (!vendorId.empty() && !other.vendorId.empty()) {
        return vendorId == other.vendorId && productId == other.productId;
    }
    return name == other.name;
}

std::vector<PortInfo> enumeratePorts() {
    // The INTERFACE class, with DIGCF_DEVICEINTERFACE — without that flag the
    // GUID is read as a SETUP class and the enumeration is silently wrong.
    // Microsoft: "Using the device interface (GUID_DEVINTERFACE_COMPORT) is the
    // recommended way to discover and access a COM port."
    const HDEVINFO set = ::SetupDiGetClassDevsW(&GUID_DEVINTERFACE_COMPORT, nullptr, nullptr,
                                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::vector<PortInfo> ports;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; ::SetupDiEnumDeviceInfo(set, index, &device) != FALSE; ++index) {
        PortInfo port;
        port.name = portName(set, &device);
        if (port.name.empty()) {
            continue;  // no PortName: not a port we can open
        }
        port.friendlyName = narrow(firstString(deviceProperty(set, &device, SPDRP_FRIENDLYNAME)));
        for (const std::wstring& id : multiString(deviceProperty(set, &device, SPDRP_HARDWAREID))) {
            if (parseUsbIds(narrow(id), &port.vendorId, &port.productId)) {
                break;
            }
        }
        ports.push_back(std::move(port));
    }
    ::SetupDiDestroyDeviceInfoList(set);

    std::ranges::sort(ports, [](const PortInfo& a, const PortInfo& b) { return a.name < b.name; });
    return ports;
}

}  // namespace krait::net::serial
