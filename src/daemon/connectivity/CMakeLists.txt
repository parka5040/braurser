find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

add_library(braurser-daemon-connectivity STATIC checker.cpp)
target_include_directories(braurser-daemon-connectivity PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-connectivity PUBLIC braurser-daemon-core PkgConfig::GIO)
braurser_harden(braurser-daemon-connectivity)
