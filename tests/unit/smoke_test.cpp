#include "core/version.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("krait-core links and reports its version", "[smoke]") {
    CHECK(krait::core::versionString() == "0.0.1");
}
