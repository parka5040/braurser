braurser_add_test(NAME news-tests PREFIX News SOURCES feed_test.cpp LIBS braurser-daemon-news)
target_compile_definitions(news-tests PRIVATE FIXTURES_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures")
