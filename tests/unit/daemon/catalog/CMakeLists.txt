braurser_add_test(NAME catalog-tests PREFIX Catalog SOURCES catalog_test.cpp LIBS braurser-daemon-catalog)
target_compile_definitions(catalog-tests PRIVATE CATALOG_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/catalog")
braurser_add_test(NAME local-query-tests PREFIX LocalQuery SOURCES query_test.cpp LIBS braurser-daemon-catalog)
