braurser_add_test(NAME updates-tests PREFIX Update SOURCES updates_test.cpp LIBS braurser-daemon-updates)
target_compile_definitions(updates-tests PRIVATE
    UPDATES_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/updates"
    RESTART_RULES="${PROJECT_SOURCE_DIR}/data/restart-rules.json")
