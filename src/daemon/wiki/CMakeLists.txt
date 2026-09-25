find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
pkg_check_modules(GUMBO REQUIRED IMPORTED_TARGET gumbo)

add_library(braurser-daemon-wiki STATIC sanitizer.cpp cache.cpp client.cpp)
target_include_directories(braurser-daemon-wiki PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-wiki PUBLIC braurser-daemon-core braurser-validate nlohmann_json::nlohmann_json PkgConfig::GUMBO)
braurser_harden(braurser-daemon-wiki)
