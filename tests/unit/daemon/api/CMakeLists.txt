braurser_add_test(NAME api-tests PREFIX Api SOURCES api_test.cpp LIBS braurser-daemon-app)
target_include_directories(api-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/daemon/app")
