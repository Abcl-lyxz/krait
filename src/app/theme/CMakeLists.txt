# krait-theme — the theme system (M5 T75/T76).
#
# Before tests/unit for the same reason krait-settings is: the round-trip and
# import tests link it, and a target cannot be linked before it is defined.
#
# Qt6::Core only. The importers need an XML reader and a JSON reader, and both
# live in QtCore — reaching for Qt6::Xml (the SAX/DOM module) would add a
# dependency for something QXmlStreamReader already does.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
find_package(tomlplusplus CONFIG REQUIRED)

# Q_OBJECT in store.h; without AUTOMOC the vtable is undefined at link.
set(CMAKE_AUTOMOC ON)

add_library(krait-theme STATIC
    import.cpp
    import.h
    store.cpp
    store.h
    theme.cpp
    theme.h
)
target_include_directories(krait-theme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(krait-theme PUBLIC Qt6::Core)

# toml++ by INCLUDE PATH rather than by linking its target — the same trick and
# the same reason as krait-settings; see the long note in
# src/app/settings/CMakeLists.txt.
target_include_directories(krait-theme SYSTEM PRIVATE
    $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(krait-theme PRIVATE TOML_EXCEPTIONS=0 TOML_HEADER_ONLY=1)
