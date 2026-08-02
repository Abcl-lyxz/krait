# krait-crash — local minidumps (M6 T86).
#
# Its own library, before tests/unit, so the file-name and pruning logic can be
# tested. That split is the point: the handler itself runs in a process that is
# already falling over, which is the worst possible place to find out the path
# was wrong or that the prune deleted the dump it had just written.
#
# Qt6::Core only — a crash handler that needed a window could not be tested and
# would be useless for a crash before the window existed.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)

add_library(krait-crash STATIC
    crash_handler.cpp
    crash_handler.h
)
target_include_directories(krait-crash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# dbghelp for MiniDumpWriteDump. Named here rather than relying on an MSVC
# default because it is not one.
target_link_libraries(krait-crash PUBLIC Qt6::Core PRIVATE dbghelp)
