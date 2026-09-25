add_library(braurser-validate STATIC validate.cpp)
target_include_directories(braurser-validate PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_options(braurser-validate PRIVATE -fno-exceptions -fno-rtti)
braurser_harden(braurser-validate)
