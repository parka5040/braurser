braurser_add_test(NAME protocol-tests PREFIX Protocol SOURCES protocol_test.cpp LIBS braurser-protocol)
target_compile_definitions(protocol-tests PRIVATE PROTOCOL_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/protocol")
