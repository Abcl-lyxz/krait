#pragma once

#include <string_view>

namespace krait::core {

// Version of the core library, e.g. "0.0.1".
[[nodiscard]] std::string_view versionString() noexcept;

}  // namespace krait::core
