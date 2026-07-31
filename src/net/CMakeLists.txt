# The backend layer (rules/net.md). A library rather than sources compiled
# straight into krait-app, because the shared IBackend contract tests have to
# link the backends and you cannot link an executable.
#
# Qt6::Core only: a backend that needed Qt Gui to run would be a backend you
# could not test without a display, and net.md's contract suite is the point.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
# ADR-0002: libssh, DYNAMICALLY linked (LGPL-2.1). The vcpkg x64-windows
# triplet builds it as a DLL, which is what keeps relink ability — a static
# libssh here would be a licensing change, not a build tweak.
find_package(libssh CONFIG REQUIRED)

# Q_OBJECT in ibackend.h and conpty_backend.h; without AUTOMOC the vtables are
# undefined at link (same reason as src/app/settings).
set(CMAKE_AUTOMOC ON)

add_library(krait-net STATIC
    error.h
    ibackend.h
    reconnect.h
    remote_text.cpp
    remote_text.h
    conpty/conpty_backend.cpp
    conpty/conpty_backend.h
    ssh/hostkey_art.cpp
    ssh/hostkey_art.h
    ssh/ssh_backend.cpp
    ssh/ssh_backend.h
    vault/vault.cpp
    vault/vault.h
)

target_include_directories(krait-net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Crypt32 for the DPAPI vault (CryptProtectData lives in Crypt32.dll).
target_link_libraries(krait-net PUBLIC Qt6::Core PRIVATE Crypt32 ssh)
