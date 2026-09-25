braurser_add_test(NAME wiki-tests PREFIX Wiki SOURCES wiki_test.cpp LIBS braurser-daemon-wiki)
braurser_add_test(NAME sanitizer-tests PREFIX Sanitizer SOURCES sanitizer_test.cpp LIBS braurser-daemon-wiki)
target_compile_definitions(wiki-tests PRIVATE FIXTURES_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures")
target_compile_definitions(sanitizer-tests PRIVATE FIXTURES_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures")
