braurser_add_test(NAME launcher-tests PREFIX Launcher SOURCES launcher_test.cpp LIBS braurser-launcher-selection)
target_compile_definitions(launcher-tests PRIVATE
    BRAURSER_DESKTOP="${PROJECT_BINARY_DIR}/src/launcher/braurser.desktop"
    BRAURSER_NOTIFIER_DESKTOP="${PROJECT_BINARY_DIR}/src/notifier/braurser-notifier.desktop")
braurser_add_test(NAME notifier-tests PREFIX Notifier SOURCES service_test.cpp LIBS braurser-daemon-notifications braurser-notifier-policy)
