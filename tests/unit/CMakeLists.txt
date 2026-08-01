find_package(Catch2 3 CONFIG REQUIRED)
# Imported targets are directory-scoped, so src/render finding Qt does not make
# Qt6::Gui visible here — and the DL_PATHS below needs it by name.
# Qt6::Test is for QSignalSpy only — the settings tests assert which signals
# fired, and hand-rolling that is more code than the dependency.
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui Qml ShaderTools Test Xml)

add_executable(krait-core-tests
    atlas_test.cpp
    cli_test.cpp
    grid_test.cpp
    modes_test.cpp
    palette_test.cpp
    osc_test.cpp
    profile_test.cpp
    putty_import_test.cpp
    ssh_config_import_test.cpp
    reflow_test.cpp
    scrollback_test.cpp
    search_test.cpp
    fontdb_test.cpp
    frame_builder_test.cpp
    session_test.cpp
    shaper_test.cpp
    smoke_test.cpp
    triggers_test.cpp
    width_test.cpp
)
# krait-shaper brings krait-core with it (PUBLIC), and stays Qt-free so real
# HarfBuzz shaping is testable here without a QGuiApplication.
target_link_libraries(krait-core-tests
    PRIVATE krait-core krait-shaper krait-session Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(krait-core-tests)

# The Qt-linked test binary (T26, T27). Separate from krait-core-tests, which
# stays Qt-free on purpose so the shaper is provably testable without a
# QGuiApplication — linking even Qt6::Core into it costs that property, since
# the binary then will not start without Qt6Core.dll beside it.
add_executable(krait-qt-tests
    action_labels_test.cpp
    agent_bridge_test.cpp
    ../../src/app/session_model.cpp
    ../../src/app/sftp_model.cpp
    sftp_panel_test.cpp
    ../../src/app/backend_factory.cpp
    ../../src/app/mremoteng_import.cpp
    mremoteng_import_test.cpp
    ../../src/app/capture.cpp
    capture_test.cpp
    backend_contract_test.cpp
    backend_factory_test.cpp
    error_banner_test.cpp
    gpu_resources_test.cpp
    hostkey_art_test.cpp
    i18n_test.cpp
    copy_mode_test.cpp
    ime_test.cpp
    keymap_test.cpp
    kitty_keys_test.cpp
    paste_test.cpp
    reconnect_test.cpp
    remote_text_test.cpp
    settings_test.cpp
    ../../src/app/taskbar_progress.cpp
    taskbar_progress_test.cpp
    forwards_test.cpp
    socks5_test.cpp
    raw_contract_test.cpp
    serial_test.cpp
    sftp_test.cpp
    ssh_contract_test.cpp
    telnet_contract_test.cpp
    telnet_test.cpp
    ../support/telnet_test_server.cpp
    ../support/telnet_test_server.h
    ../support/ssh_test_server.cpp
    ../support/ssh_test_server.h
    vault_test.cpp
)
# The contract tests run a real SSH server in-process (see the note in
# ssh_test_server.h for why that beats a Docker fixture here), so this binary
# needs libssh directly rather than only through krait-net.
find_package(libssh CONFIG REQUIRED)
# session_model.cpp is compiled straight in rather than linked: it lives in
# krait-app, which is an executable, and the label-parity test has to see it.
target_include_directories(krait-qt-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../support
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/app)
# ws2_32 for the test server's wake-the-accept self-connect (see wakeAccept).
# ole32 for CoCreateInstance(CLSID_TaskbarList): taskbar_progress.cpp is
# compiled in for the aggregation test the same way session_model.cpp is.
target_link_libraries(krait-qt-tests PRIVATE ssh ws2_32 ole32)
target_link_libraries(krait-qt-tests
    PRIVATE krait-render krait-input krait-settings krait-session krait-net Qt6::Test Qt6::Qml Qt6::Xml
            Catch2::Catch2WithMain)
# On this target only. backend_contract_test.cpp declares a Q_OBJECT subclass of
# IBackend and needs moc; krait-core-tests deliberately links no Qt at all, so a
# directory-wide set(CMAKE_AUTOMOC ON) would be aimed at the wrong binary.
set_target_properties(krait-qt-tests PROPERTIES AUTOMOC ON)

# The real shaders, not stubs: QRhi's FRONTEND rejects an empty QShader
# ("Empty shader passed to graphics pipeline") before any backend sees it, even
# the Null one. Compiling them here also means the test fails if a shader stops
# compiling, which the app build would only catch at startup.
qt_add_shaders(krait-qt-tests "krait-qt-test-shaders"
    PREFIX "/shaders"
    FILES
        ../../src/render/shaders/cell.vert
        ../../src/render/shaders/cell.frag
        ../../src/render/shaders/glyph.vert
        ../../src/render/shaders/glyph.frag
    OUTPUTS
        cell.vert.qsb
        cell.frag.qsb
        glyph.vert.qsb
        glyph.frag.qsb
)
# DL_PATHS, not a PATH set on the tests: Catch2 RUNS the binary at build time to
# enumerate cases, so without Qt6Gui.dll findable then, the build fails with
# "Error listing tests from executable" long before ctest gets a turn.
# The i18n gate reads the .ts files from the SOURCE tree, the same way the
# corpus harness reads its .case files: the rule is about what is committed,
# not about what a build happened to generate.
target_compile_definitions(krait-qt-tests
    PRIVATE KRAIT_I18N_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../src/app/i18n")

catch_discover_tests(krait-qt-tests DL_PATHS "$<TARGET_FILE_DIR:Qt6::Gui>")
