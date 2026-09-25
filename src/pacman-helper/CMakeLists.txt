add_library(braurser-pacman-request STATIC request.cpp)
target_include_directories(braurser-pacman-request PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(braurser-pacman-request PUBLIC braurser-validate)
target_compile_options(braurser-pacman-request PRIVATE -fno-exceptions -fno-rtti)
braurser_harden(braurser-pacman-request)

add_executable(braurser-pacman-helper main.cpp)
target_link_libraries(braurser-pacman-helper PRIVATE braurser-pacman-request)
target_compile_options(braurser-pacman-helper PRIVATE -fno-exceptions -fno-rtti)
target_link_options(braurser-pacman-helper PRIVATE -Wl,--as-needed -static-libgcc)
braurser_harden(braurser-pacman-helper)

set(BRAURSER_HELPER_PATH "${CMAKE_INSTALL_PREFIX}/libexec/braurser/braurser-pacman-helper" CACHE STRING "Installed pacman helper absolute path")
if(NOT BRAURSER_HELPER_PATH MATCHES "^/[-a-zA-Z0-9_./]+/braurser-pacman-helper$" OR BRAURSER_HELPER_PATH MATCHES "/[.][.]?(/|$)")
    message(FATAL_ERROR "BRAURSER_HELPER_PATH must be a safe absolute helper path")
endif()
get_filename_component(BRAURSER_HELPER_DIRECTORY "${BRAURSER_HELPER_PATH}" DIRECTORY)
install(TARGETS braurser-pacman-helper RUNTIME DESTINATION "${BRAURSER_HELPER_DIRECTORY}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/data")
configure_file("${PROJECT_SOURCE_DIR}/data/org.braurser.pacman.policy.in"
    "${CMAKE_BINARY_DIR}/data/org.braurser.pacman.policy" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/data/org.braurser.pacman.policy"
    DESTINATION /usr/share/polkit-1/actions)
