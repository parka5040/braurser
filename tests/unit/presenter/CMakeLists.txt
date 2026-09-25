braurser_add_test(NAME presenter-tests PREFIX Presenter
    SOURCES copy_test.cpp models_test.cpp rpc_test.cpp
    LIBS braurser-presenter)
target_compile_definitions(presenter-tests PRIVATE
    BRAURSER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/protocol"
    BRAURSER_SOURCE="${CMAKE_SOURCE_DIR}")
