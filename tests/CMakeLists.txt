add_test(NAME CommentPolicy.Repository
    COMMAND bash "${CMAKE_SOURCE_DIR}/tests/comment-policy.sh" "${CMAKE_SOURCE_DIR}")

braurser_component(tests/unit/validate)
braurser_component(tests/unit/domain)
braurser_component(tests/unit/protocol)
braurser_component(tests/unit/presenter)
braurser_component(tests/unit/daemon)
braurser_component(tests/unit/gtk)
braurser_component(tests/unit/qt)
braurser_component(tests/security)
braurser_component(tests/fuzz)
braurser_component(tests/theme)
include("${CMAKE_CURRENT_SOURCE_DIR}/e2e/rpc-client.cmake")
