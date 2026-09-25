find_package(PkgConfig REQUIRED)
pkg_check_modules(LAUNCHER_GIOMM REQUIRED IMPORTED_TARGET giomm-2.68)

add_library(braurser-launcher-selection STATIC selection.cpp)
target_include_directories(braurser-launcher-selection PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
braurser_harden(braurser-launcher-selection)
add_executable(braurser main.cpp)
target_link_libraries(braurser PRIVATE braurser-launcher-selection braurser-presenter PkgConfig::LAUNCHER_GIOMM)
braurser_harden(braurser)
install(TARGETS braurser RUNTIME DESTINATION bin)

set(BRAURSER_BINDIR "${CMAKE_INSTALL_PREFIX}/bin")
configure_file("${PROJECT_SOURCE_DIR}/data/braurser.desktop.in" "${CMAKE_CURRENT_BINARY_DIR}/braurser.desktop" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/braurser.desktop" DESTINATION share/applications)
