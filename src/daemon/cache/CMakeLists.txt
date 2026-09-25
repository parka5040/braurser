find_package(SQLite3 REQUIRED)

add_library(braurser-daemon-cache STATIC database.cpp internal.cpp snapshot.cpp search.cpp)
target_include_directories(braurser-daemon-cache PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-cache PUBLIC SQLite3::SQLite3 PRIVATE braurser-daemon-core braurser-validate)
braurser_harden(braurser-daemon-cache)
