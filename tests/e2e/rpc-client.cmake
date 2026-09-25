add_executable(braurser-rpc-client "${PROJECT_SOURCE_DIR}/tests/e2e/rpc-client.cpp")
target_link_libraries(braurser-rpc-client PRIVATE braurser-protocol)
braurser_harden(braurser-rpc-client)
