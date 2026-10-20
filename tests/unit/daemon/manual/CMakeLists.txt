braurser_add_test(NAME git-tests PREFIX Git SOURCES git_test.cpp pins_test.cpp LIBS braurser-daemon-manual)
braurser_add_test(NAME manual-tests PREFIX Manual SOURCES manual_test.cpp LIBS braurser-daemon-manual)
target_compile_definitions(manual-tests PRIVATE MANUAL_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/manual")
braurser_add_test(NAME launcher-scan-tests PREFIX LauncherScan SOURCES scanner_test.cpp LIBS braurser-daemon-manual)
