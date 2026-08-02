# Qt shell + M0 render spike (plan T11). Qt via CMAKE_PREFIX_PATH (dev
# preset). Pin is 6.11.1 (ADR-0001); floor is 6.7 (QQuickRhiItem) so the
# spike also builds against the machine's 6.10.3 while the 6.11.1 mirror
# download is flaky — flip CMAKE_PREFIX_PATH when it lands.
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui GuiPrivate Quick ShaderTools LinguistTools)
find_package(freetype CONFIG REQUIRED)

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(krait-app
    main.cpp
    ../render/spike/glyph_atlas.cpp
    ../render/spike/glyph_atlas.h
)
# Console subsystem for M0: the verify gates read the log lines from stdout.
# The shipping subsystem choice is revisited with the real app shell.
set_property(TARGET krait-app PROPERTY WIN32_EXECUTABLE FALSE)

qt_add_qml_module(krait-app
    URI Krait
    VERSION 1.0
    QML_FILES
        qml/Banner.qml
        qml/BannerButton.qml
        qml/BroadcastBar.qml
        qml/FilePanel.qml
        qml/Main.qml
        qml/Palette.qml
        qml/SessionPane.qml
        qml/Settings.qml
        qml/SnippetBar.qml
        qml/TabStrip.qml
    SOURCES
        backend_factory.cpp
        broadcast.cpp
        broadcast.h
        capture.cpp
        capture.h
        backend_factory.h
        quake.cpp
        quake.h
        mremoteng_import.cpp
        mremoteng_import.h
        notifier.cpp
        notifier.h
        session_model.cpp
        settings_model.cpp
        settings_model.h
        session_model.h
        sftp_model.cpp
        sftp_model.h
        shell_integration.cpp
        shell_integration.h
        taskbar_progress.cpp
        taskbar_progress.h
        terminal_item.cpp
        terminal_item.h
        ../render/spike/grid_item.cpp
        ../render/spike/grid_item.h
        ../render/spike/triangle_item.cpp
        ../render/spike/triangle_item.h
)

qt_add_shaders(krait-app "krait-spike-shaders"
    PREFIX "/shaders"
    FILES
        ../render/shaders/triangle.vert
        ../render/shaders/triangle.frag
        ../render/shaders/grid.vert
        ../render/shaders/grid.frag
        ../render/shaders/cell.vert
        ../render/shaders/cell.frag
        ../render/shaders/glyph.vert
        ../render/shaders/glyph.frag
    OUTPUTS
        triangle.vert.qsb
        triangle.frag.qsb
        grid.vert.qsb
        grid.frag.qsb
        cell.vert.qsb
        cell.frag.qsb
        glyph.vert.qsb
        glyph.frag.qsb
)

# The generated qmltyperegistrations includes module headers by basename.
target_include_directories(krait-app PRIVATE ../render/spike)

# GuiPrivate: QRhi is Qt's semi-public "guaranteed within a minor series"
# API; its headers (rhi/qrhi.h) ship under the private include umbrella.
# krait-shaper carries krait-core, HarfBuzz, FreeType and DirectWrite: the whole
# T23/T24/T25 render pipeline the spike path was replaced with.
# ole32 for CoCreateInstance(CLSID_TaskbarList) — the OSC 9;4 taskbar bar (T67).
# uuid.lib, which carries the CLSID/IID symbols, is one of MSVC's defaults.
# user32 (RegisterHotKey/SetForegroundWindow, quake.cpp) is another MSVC default.
target_link_libraries(krait-app
    PRIVATE krait-core krait-render krait-input krait-settings krait-session krait-net Qt6::Quick Qt6::GuiPrivate
            freetype ole32
            # shell32 for Shell_NotifyIconW (notifier.cpp) — the library
            # learn.microsoft.com names for it, alongside ole32's CoCreateInstance
            # for the taskbar.
            shell32)

# Translations (plan T32). EN and TH ship together: rules/ui.md makes Thai a
# first-class locale, not an afterthought, so a string that lands without both
# is incomplete work.
#
# SOURCE_TARGETS is explicit rather than left to automatic collection: the
# automatic path scans every target in the project, which would sweep in the
# test binaries and put Catch2 assertion text in the translator's file.
#
# The .qm files are embedded under ":/i18n", so a portable copy carries its own
# translations instead of depending on files next to the exe.
qt_add_translations(krait-app
    SOURCE_TARGETS krait-app krait-input krait-settings krait-session krait-net
    TS_FILES
        i18n/krait_en.ts
        i18n/krait_th.ts
    # error_banner.h is header-only, so it belongs to no target's source list
    # and lupdate would never see it — every backend error message would ship
    # untranslated with nothing to show for it.
    SOURCES error_banner.h
    RESOURCE_PREFIX "/i18n"
    # -locations none strips the <location filename= line=> attributes.
    #
    # They are the reason the catalogues could not be checked mechanically:
    # every unrelated edit that shifts a line rewrites dozens of them, so
    # "re-run lupdate and see if the .ts files changed" was pure noise and
    # nobody could run it. Without locations the only thing a regeneration
    # changes is the set of STRINGS, which is exactly the thing worth gating.
    LUPDATE_OPTIONS -locations none
)

# ADR-0011: the bundled ConPTY rides next to the exe; the backend refuses
# to run without it.
add_custom_command(TARGET krait-app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:krait-app>/openconsole
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openconsole/conpty.dll
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openconsole/OpenConsole.exe
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openconsole/LICENSE
        $<TARGET_FILE_DIR:krait-app>/openconsole
)

# T73. The shell-integration scripts ride next to the exe the same way, and for
# the same reason they are FILES rather than a Qt resource: the SSH installer
# uploads one of them verbatim, and a user with a local shell has to be able to
# point `source` at a real path. shell_integration.cpp resolves them relative to
# applicationDirPath().
add_custom_command(TARGET krait-app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:krait-app>/shell-integration
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/shell-integration/krait.bash
        ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/shell-integration/krait.zsh
        ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/shell-integration/krait.fish
        ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/shell-integration/krait.ps1
        $<TARGET_FILE_DIR:krait-app>/shell-integration
)
