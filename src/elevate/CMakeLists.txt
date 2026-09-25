set(BRAURSER_HELPER_PATH "${CMAKE_INSTALL_PREFIX}/libexec/braurser/braurser-pacman-helper" CACHE STRING "Installed pacman helper absolute path")

add_library(braurser-elevate-core STATIC normalize.cpp auth.cpp)
target_include_directories(braurser-elevate-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(braurser-elevate-core PUBLIC braurser-pacman-request)
braurser_harden(braurser-elevate-core)

add_executable(braurser-elevate main.cpp)
target_link_libraries(braurser-elevate PRIVATE braurser-elevate-core)
target_compile_definitions(braurser-elevate PRIVATE BRAURSER_HELPER_PATH="${BRAURSER_HELPER_PATH}")
braurser_harden(braurser-elevate)
install(TARGETS braurser-elevate RUNTIME DESTINATION libexec/braurser)
