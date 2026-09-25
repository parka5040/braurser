cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(braurser_theme_tests LANGUAGES NONE)
    enable_testing()
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Theme.cmake")
    braurser_generate_theme(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    braurser_rasterize_icons(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/icons")
endif()

find_program(BRAURSER_XMLLINT NAMES xmllint REQUIRED)
file(GLOB theme_icons CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../data/icons/*.svg")
if(NOT theme_icons)
    message(FATAL_ERROR "Theme SVG assets are missing")
endif()

add_test(NAME Theme.Generate
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/theme-check"
        -P "${CMAKE_CURRENT_LIST_DIR}/generate.cmake")
add_test(NAME Theme.SvgValid
    COMMAND "${BRAURSER_XMLLINT}" --noout --nonet ${theme_icons})
add_test(NAME Theme.SvgSafe
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/svg_safe.cmake")
add_test(NAME Theme.Contrast
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/contrast.cmake")
add_test(NAME Theme.RejectMalformed
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/theme-invalid"
        -P "${CMAKE_CURRENT_LIST_DIR}/reject_malformed.cmake")
set_tests_properties(Theme.Generate Theme.SvgValid Theme.SvgSafe Theme.Contrast Theme.RejectMalformed
    PROPERTIES TIMEOUT 30 LABELS theme)
