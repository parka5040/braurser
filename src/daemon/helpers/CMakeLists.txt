add_library(braurser-daemon-helpers STATIC aur_helper.cpp)
target_include_directories(braurser-daemon-helpers PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-daemon-helpers PUBLIC braurser-daemon-core braurser-domain braurser-validate)
braurser_harden(braurser-daemon-helpers)
