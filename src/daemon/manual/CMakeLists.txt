find_package(PkgConfig REQUIRED)
pkg_check_modules(MANUAL_GIT REQUIRED IMPORTED_TARGET libgit2)
pkg_check_modules(MANUAL_XML REQUIRED IMPORTED_TARGET libxml-2.0)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3 REQUIRED)

add_library(braurser-daemon-manual STATIC
    git.cpp
    transport.cpp
    update.cpp
    tree.cpp
    stage.cpp
    install.cpp
    package.cpp
    scanner.cpp
)
target_include_directories(braurser-daemon-manual PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-manual PUBLIC braurser-daemon-core braurser-daemon-catalog braurser-validate PkgConfig::MANUAL_GIT PkgConfig::MANUAL_XML CURL::libcurl nlohmann_json::nlohmann_json PRIVATE braurser-protocol)
braurser_harden(braurser-daemon-manual)
