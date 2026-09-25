if(TARGET braurser-gtk-kit-tests)
    return()
endif()
braurser_add_test(NAME braurser-gtk-kit-tests PREFIX GtkKit
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/document_test.cpp" "${CMAKE_CURRENT_LIST_DIR}/resource_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/widgets_test.cpp"
    LIBS braurser-gtk-kit)
target_compile_definitions(braurser-gtk-kit-tests PRIVATE
    BRAURSER_KIT_ICON_DIRECTORY="${CMAKE_SOURCE_DIR}/data/icons")
add_subdirectory(app)
braurser_add_test(NAME braurser-presenter-fixtures-tests PREFIX Presenter
    SOURCES "${CMAKE_SOURCE_DIR}/tests/unit/presenter/fixtures_test.cpp"
    LIBS braurser-presenter-fixtures)
