# krait-input — key translation and mouse report encoding (plan T27).
#
# Its own target, and added to the build BEFORE tests/unit, for one reason: the
# plan's verification for T27 is "unit: keymap table cases", and a keymap table
# is only testable if translating a key needs no window, no pty and no session.
# Qt6::Core alone (QByteArray, Qt::Key) — no Gui, no Quick.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)

add_library(krait-input STATIC
    keymap.cpp
    mouse.cpp
    paste.cpp
)
target_include_directories(krait-input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# krait-core PUBLIC: mouse.h takes a core::vt::Grid to read the tracking mode.
target_link_libraries(krait-input PUBLIC krait-core Qt6::Core)
