find_package(GTest REQUIRED)
include(GoogleTest)

function(braurser_add_test)
    cmake_parse_arguments(ARG "" "NAME;PREFIX" "SOURCES;LIBS" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_PREFIX OR NOT ARG_SOURCES OR ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "braurser_add_test requires NAME, PREFIX, and SOURCES")
    endif()

    add_executable(${ARG_NAME} ${ARG_SOURCES})
    braurser_harden(${ARG_NAME})
    target_link_libraries(${ARG_NAME} PRIVATE GTest::gtest_main ${ARG_LIBS})
    gtest_discover_tests(${ARG_NAME}
        TEST_PREFIX "${ARG_PREFIX}."
        DISCOVERY_TIMEOUT 30
    )
endfunction()

function(braurser_add_fuzzer)
    if(NOT BRAURSER_BUILD_FUZZERS OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        return()
    endif()

    cmake_parse_arguments(ARG "" "NAME" "SOURCES;LIBS" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_SOURCES OR ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "braurser_add_fuzzer requires NAME and SOURCES")
    endif()

    add_executable(${ARG_NAME} ${ARG_SOURCES})
    set_property(TARGET ${ARG_NAME} PROPERTY BRAURSER_FUZZER TRUE)
    braurser_harden(${ARG_NAME})
    target_compile_options(${ARG_NAME} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${ARG_NAME} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_LIBS})
endfunction()
