find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
pkg_check_modules(NOTIFICATION_GIOMM REQUIRED IMPORTED_TARGET giomm-2.68)

add_library(braurser-daemon-notifications STATIC service.cpp registration.cpp)
target_include_directories(braurser-daemon-notifications PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-notifications PUBLIC braurser-daemon-api braurser-daemon-core braurser-presenter PkgConfig::NOTIFICATION_GIOMM nlohmann_json::nlohmann_json)
braurser_harden(braurser-daemon-notifications)
