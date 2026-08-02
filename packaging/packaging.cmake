# Krait packaging — portable ZIP + NSIS installer (T87, ADR-0007).
#
# Included from the TOP-LEVEL CMakeLists after every add_subdirectory(), because
# CPack collects the install() rules that exist when include(CPack) runs.

# The portable marker. ADR-0007 wants the ZIP to be portable out of the box —
# config beside the exe, nothing written to %APPDATA% — and settings/paths.cpp
# decides that by the PRESENCE OF THIS FILE, never by whether a config happens
# to sit there. An installed copy must NOT get one, so this is an option the
# release job flips rather than something baked into the install tree: cpack -G
# ZIP with it ON, cpack -G NSIS with it OFF. One option beats teaching the NSIS
# script to delete a file the ZIP needs.
option(KRAIT_PORTABLE_PACKAGE "Ship the krait.portable marker in the package" OFF)
if(KRAIT_PORTABLE_PACKAGE)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/krait.portable" "")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/krait.portable" DESTINATION bin)
endif()

set(CPACK_PACKAGE_NAME "Krait")
set(CPACK_PACKAGE_VENDOR "Krait")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "krait-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Krait")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "A GPU-accelerated SSH client and terminal for Windows")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_LIST_DIR}/../LICENSE")
set(CPACK_VERBATIM_VARIABLES ON)

# ZIP always: it needs no external tool, so the portable artefact can never be
# the one that silently stops being built.
set(CPACK_GENERATOR "ZIP")

# NSIS only when makensis is actually here. A hard requirement would break
# `cmake --preset dev` on every machine without NSIS installed — including CI,
# which has no reason to carry it — and CPack's NSIS generator needs 3.03+.
find_program(KRAIT_MAKENSIS makensis)
if(KRAIT_MAKENSIS)
    list(APPEND CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_EXECUTABLE "${KRAIT_MAKENSIS}")
    set(CPACK_NSIS_PACKAGE_NAME "Krait")
    set(CPACK_NSIS_DISPLAY_NAME "Krait")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY "bin")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/Abcl-lyxz/krait")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "krait-app.exe")
    # Per-user, per ADR-0007. This sets the default PATH only — see the
    # "Still missing" section of packaging/README.md: CPack's stock NSIS
    # template hardcodes `RequestExecutionLevel admin`, so the installer still
    # ELEVATES until that template is overridden. The winget manifest claims
    # Scope: user, so the two disagree today and that is a release blocker.
    set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA\\Programs")
    # OFF deliberately: it is an ASK, and an ask cannot be answered under /S.
    # ADR-0007 requires silent-capable, so an upgrade installs over the top
    # rather than prompting nobody.
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)
    # A terminal is not a PATH tool, and the extra page is one more thing to
    # answer in an install that should be Next-Next.
    set(CPACK_NSIS_MODIFY_PATH OFF)
else()
    message(STATUS
        "packaging: makensis not found - ZIP only. Install NSIS 3.03+ for the installer.")
endif()

include(CPack)
