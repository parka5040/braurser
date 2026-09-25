find_package(PkgConfig REQUIRED)
pkg_check_modules(CATALOG_ARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(CATALOG_XML REQUIRED IMPORTED_TARGET libxml-2.0)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json 3 REQUIRED)

add_library(braurser-daemon-catalog STATIC
    pacman.cpp
    aur.cpp
    appstream.cpp
    desktop.cpp
    icon.cpp
)
target_include_directories(braurser-daemon-catalog PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-catalog PUBLIC braurser-daemon-core braurser-domain braurser-validate PkgConfig::CATALOG_ARCHIVE PkgConfig::CATALOG_XML ZLIB::ZLIB nlohmann_json::nlohmann_json)
braurser_harden(braurser-daemon-catalog)
