add_library(braurser-daemon-elevation STATIC selector.cpp)
target_include_directories(braurser-daemon-elevation PUBLIC "${PROJECT_SOURCE_DIR}/include")
braurser_harden(braurser-daemon-elevation)
