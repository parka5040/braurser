find_package(nlohmann_json 3 REQUIRED)

add_library(braurser-daemon-updates STATIC
    temp_sync_db.cpp
    check.cpp
    rules.cpp
    scanner.cpp
    advisor.cpp
)
target_include_directories(braurser-daemon-updates PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-updates PUBLIC
    braurser-daemon-core braurser-daemon-catalog braurser-daemon-helpers
    braurser-domain braurser-validate nlohmann_json::nlohmann_json)
braurser_harden(braurser-daemon-updates)
