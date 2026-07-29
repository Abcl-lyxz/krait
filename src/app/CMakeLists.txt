# Qt shell + M0 render spike (plan T11). Qt via CMAKE_PREFIX_PATH (dev
# preset). Pin is 6.11.1 (ADR-0001); floor is 6.7 (QQuickRhiItem) so the
# spike also builds against the machine's 6.10.3 while the 6.11.1 mirror
# download is flaky — flip CMAKE_PREFIX_PATH when it lands.
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui GuiPrivate Quick ShaderTools)
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
        qml/Main.qml
    SOURCES
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
    OUTPUTS
        triangle.vert.qsb
        triangle.frag.qsb
        grid.vert.qsb
        grid.frag.qsb
)

# The generated qmltyperegistrations includes module headers by basename.
target_include_directories(krait-app PRIVATE ../render/spike)

# GuiPrivate: QRhi is Qt's semi-public "guaranteed within a minor series"
# API; its headers (rhi/qrhi.h) ship under the private include umbrella.
target_link_libraries(krait-app PRIVATE Qt6::Quick Qt6::GuiPrivate freetype)
