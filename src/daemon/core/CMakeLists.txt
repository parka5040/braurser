find_package(CURL REQUIRED)

add_library(braurser-daemon-core STATIC
    process.cpp
    connect.cpp
    http.cpp
    files.cpp
    text.cpp
    logger.cpp
)
target_include_directories(braurser-daemon-core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(braurser-daemon-core PRIVATE _GNU_SOURCE)
target_link_libraries(braurser-daemon-core PUBLIC CURL::libcurl)
braurser_harden(braurser-daemon-core)
