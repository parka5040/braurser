find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

set(resource_dir "${CMAKE_CURRENT_BINARY_DIR}/resources")
file(MAKE_DIRECTORY "${resource_dir}")
file(GLOB kit_icons CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/icons/*.svg")
set(resource_xml "<gresources><gresource prefix=\"/org/braurser/kit\">\n")
foreach(mode IN ITEMS light dark)
    string(APPEND resource_xml "<file alias=\"braurser-${mode}.css\">${CMAKE_BINARY_DIR}/generated/theme/braurser-${mode}.css</file>\n")
endforeach()
foreach(icon IN LISTS kit_icons)
    get_filename_component(name "${icon}" NAME)
    string(APPEND resource_xml "<file alias=\"icons/scalable/actions/${name}\">${icon}</file>\n")
endforeach()
string(APPEND resource_xml "</gresource></gresources>\n")
file(WRITE "${resource_dir}/kit.gresource.xml" "${resource_xml}")
add_custom_command(OUTPUT "${resource_dir}/kit-resources.c"
    COMMAND "${GLIB_COMPILE_RESOURCES}" "${resource_dir}/kit.gresource.xml"
        --generate-source --manual-register --c-name braurser_kit
        --target "${resource_dir}/kit-resources.c"
    DEPENDS "${resource_dir}/kit.gresource.xml" ${kit_icons}
        "${CMAKE_BINARY_DIR}/generated/theme/braurser-light.css"
        "${CMAKE_BINARY_DIR}/generated/theme/braurser-dark.css"
    VERBATIM)
add_library(braurser-gtk-resources OBJECT "${resource_dir}/kit-resources.c")
target_link_libraries(braurser-gtk-resources PRIVATE PkgConfig::GTKMM)
add_library(braurser-gtk-kit STATIC
    theme.cpp icons.cpp widgets.cpp document.cpp app/render.cpp $<TARGET_OBJECTS:braurser-gtk-resources>)
target_include_directories(braurser-gtk-kit PUBLIC "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_BINARY_DIR}/generated/theme")
target_link_libraries(braurser-gtk-kit PUBLIC PkgConfig::GTKMM)
braurser_harden(braurser-gtk-kit)
add_executable(braurser-gtk-gallery gallery.cpp)
target_link_libraries(braurser-gtk-gallery PRIVATE braurser-gtk-kit)
braurser_harden(braurser-gtk-gallery)
add_subdirectory(app)
target_link_libraries(braurser-gtk-gallery PRIVATE braurser-gtk-app braurser-presenter-fixtures)
