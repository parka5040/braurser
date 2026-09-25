find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)

add_library(braurser-daemon-news STATIC feed.cpp)
target_include_directories(braurser-daemon-news PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-news PUBLIC braurser-daemon-core braurser-daemon-wiki PkgConfig::LIBXML2)
braurser_harden(braurser-daemon-news)
