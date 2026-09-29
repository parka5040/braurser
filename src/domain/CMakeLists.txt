add_library(braurser-domain STATIC types.cpp vercmp.cpp variants.cpp restart.cpp protection.cpp)
target_include_directories(braurser-domain PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-domain PUBLIC braurser-validate)
braurser_harden(braurser-domain)
