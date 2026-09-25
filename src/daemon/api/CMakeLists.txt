find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
pkg_check_modules(GIOMM REQUIRED IMPORTED_TARGET giomm-2.68)

add_library(braurser-daemon-api STATIC registry.cpp server.cpp handlers.cpp logger.cpp)
target_include_directories(braurser-daemon-api PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-api PUBLIC braurser-protocol braurser-daemon-core PkgConfig::GIOMM nlohmann_json::nlohmann_json)
braurser_harden(braurser-daemon-api)
