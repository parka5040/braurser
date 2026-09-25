braurser_add_test(NAME braurser-gtk-app-tests PREFIX GtkApp
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/app_test.cpp"
    LIBS braurser-gtk-app braurser-presenter-fixtures)
