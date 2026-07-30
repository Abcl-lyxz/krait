# krait-settings — the settings registry (plan T30).
#
# Added before tests/unit so the round-trip, migration and hot-reload tests can
# link it. Qt6::Core only: the registry is a QObject for its signals, and a
# settings store that needed a window could not be tested at all.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
find_package(tomlplusplus CONFIG REQUIRED)

# Q_OBJECT in registry.h; without AUTOMOC the vtable is undefined at link.
set(CMAKE_AUTOMOC ON)

add_library(krait-settings STATIC
    paths.cpp
    paths.h
    registry.cpp
    registry.h
    schema.cpp
    schema.h
)
target_include_directories(krait-settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(krait-settings PUBLIC Qt6::Core)

# toml++ by INCLUDE PATH, deliberately not by linking its target.
#
# rules/cpp.md: no exceptions across module boundaries. toml++ honours that with
# TOML_EXCEPTIONS=0 — but that macro moves the whole library into a different
# inline namespace (toml::v3::noex), and vcpkg's prebuilt tomlplusplus.lib is
# compiled with exceptions ON. Linking it while asking for the no-exceptions API
# fails with an unresolved toml::v3::noex::parse that reads like a missing
# dependency and is not one.
#
# Worse, the port's config sets -DTOML_HEADER_ONLY=0 -DTOML_SHARED_LIB=1 as
# compile OPTIONS rather than definitions, so they land after ours on the
# command line and win. Taking the headers alone sidesteps both: header-only is
# toml++'s default, and nothing overrides us.
target_include_directories(krait-settings SYSTEM PRIVATE
    $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(krait-settings PRIVATE TOML_EXCEPTIONS=0 TOML_HEADER_ONLY=1)
