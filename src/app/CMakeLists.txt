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
        qml/ThemeGallery.qml
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
        theme_model.cpp
        theme_model.h
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
        ../render/shaders/image.frag
    OUTPUTS
        triangle.vert.qsb
        triangle.frag.qsb
        grid.vert.qsb
        grid.frag.qsb
        cell.vert.qsb
        cell.frag.qsb
        glyph.vert.qsb
        glyph.frag.qsb
        image.frag.qsb
)

# The generated qmltyperegistrations includes module headers by basename.
target_include_directories(krait-app PRIVATE ../render/spike)

# C4702 (unreachable code), on this target only, and reluctantly.
#
# It fires inside QT'S OWN HEADER, not our code: QJSEngine::fromScriptValue is
# a chain of `if constexpr (...) return ...;` with a fallback `return` after it
# (qjsengine.h:112), so whichever branch is taken makes the fallback
# unreachable. qmlcachegen instantiates it for QString and bool the moment a
# .qml file calls a C++ singleton method that returns one — which ThemeGallery
# does four times, and which any future QML will do again.
#
# The alternatives were worse: contorting ThemeModel's API to return nothing
# from saveAs()/importFile() so the instantiation never happens, or naming the
# generated .rcc/qmlcache/*.cpp paths in this file, which are Qt's internal
# layout and would break on a Qt update. This does NOT weaken the rest of the
# build — krait-core, krait-render, krait-net and everything else keep /W4 /WX
# whole, and this target is a thin shell of main() plus view-models.
target_compile_options(krait-app PRIVATE /wd4702)

# GuiPrivate: QRhi is Qt's semi-public "guaranteed within a minor series"
# API; its headers (rhi/qrhi.h) ship under the private include umbrella.
# krait-shaper carries krait-core, HarfBuzz, FreeType and DirectWrite: the whole
# T23/T24/T25 render pipeline the spike path was replaced with.
# ole32 for CoCreateInstance(CLSID_TaskbarList) — the OSC 9;4 taskbar bar (T67).
# uuid.lib, which carries the CLSID/IID symbols, is one of MSVC's defaults.
# user32 (RegisterHotKey/SetForegroundWindow, quake.cpp) is another MSVC default.
target_link_libraries(krait-app
    PRIVATE krait-core krait-render krait-input krait-settings krait-theme krait-session krait-net Qt6::Quick Qt6::GuiPrivate
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
    SOURCE_TARGETS krait-app krait-input krait-settings krait-theme krait-session krait-net
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

# --- Install rules (T87, ADR-0007) ---
#
# These stage everything Krait needs EXCEPT the Qt DLLs and QML imports; see
# the note at the bottom of this block for why those are a release-job step.
install(TARGETS krait-app RUNTIME DESTINATION bin)

# The shell-integration scripts are resolved relative to applicationDirPath(),
# so they sit beside the exe in the package exactly as they do in the build dir.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../assets/shell-integration
        DESTINATION bin)

# ADR-0011: the bundled OpenConsole rides next to the exe and the ConPTY backend
# REFUSES to run without it. Leaving it out of the package ships a terminal that
# cannot open a local shell — the POST_BUILD copy above only populates the build
# tree, and a package is not a build tree.
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openconsole/conpty.dll
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openconsole/OpenConsole.exe
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openconsole/LICENSE
        DESTINATION bin/openconsole)

# LGPL compliance (ADR-0002: libssh dynamically linked; ADR-0001: Qt likewise).
# Shipping the licence texts is the obligation, and the release job is what
# gathers them — see packaging/README.md, which is installed beside them so the
# gap is visible in the package rather than only in the repo.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/THIRD-PARTY.md
        DESTINATION licenses)

# Qt's own DLLs and the QML imports are deployed by the RELEASE JOB, not from
# here, and that is a finding rather than a preference.
#
# qt_generate_deploy_qml_app_script() is the documented way and it does not work
# in this tree: the script it writes calls qt6_deploy_qml_imports(), but the
# generated .qt/QtDeploySupport.cmake only ever loaded Qt6CoreDeploySupport,
# so install-time died with `Unknown CMake command "qt6_deploy_qml_imports"`.
# Finding Qt6::Qml explicitly, in this scope and in the top-level scope, changed
# nothing — the Qml deploy support was never registered either way. Qt's docs
# promise only that a deployed tree is "ready to be packaged - for example by
# cpack" and say nothing about CPack's staging prefix, so this is the untested
# corner they leave open rather than a mistake in the call.
#
# The release job therefore runs windeployqt against the staged tree:
#
#     windeployqt --qmldir src/app/qml --release <stage>/bin/krait-app.exe
#
# packaging/README.md records this, and records that the packaged app has not
# yet been launched. Do not paper over it by shipping a package that silently
# lacks Qt.
