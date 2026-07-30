# krait-session — saved connections and the matching the palette runs on
# (plan T37). Added before tests/unit so the inheritance, round-trip and
# ranking tests can link it.
#
# No Qt at all: a profile is plain data and fuzzyScore is a pure function, so
# the whole thing is testable without a QCoreApplication, and the PuTTY
# importer (T44) can be tested without a registry.
find_package(tomlplusplus CONFIG REQUIRED)

add_library(krait-session STATIC
    cli.cpp
    cli.h
    fuzzy.cpp
    fuzzy.h
    profile.cpp
    profile.h
    putty_import.cpp
    putty_import.h
)
target_include_directories(krait-session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# toml++ by include path, never by linking its target — see the long note in
# ../settings/CMakeLists.txt. The port's prebuilt library is compiled WITH
# exceptions, and TOML_EXCEPTIONS=0 moves the API into toml::v3::noex.
target_include_directories(krait-session SYSTEM PRIVATE
    $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(krait-session PRIVATE TOML_EXCEPTIONS=0 TOML_HEADER_ONLY=1)
