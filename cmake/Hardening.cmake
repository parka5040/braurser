include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)

set(BRAURSER_SANITIZE "" CACHE STRING "Sanitizers (empty or address;undefined)")
set_property(CACHE BRAURSER_SANITIZE PROPERTY STRINGS "" "address;undefined")
if(NOT "${BRAURSER_SANITIZE}" STREQUAL "" AND
   NOT "${BRAURSER_SANITIZE}" STREQUAL "address;undefined")
    message(FATAL_ERROR "BRAURSER_SANITIZE must be empty or address;undefined")
endif()

function(braurser_harden target)
    set(compile_flags
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wconversion
        -Wshadow
        -fstack-protector-strong
        -fstack-clash-protection
        -fPIE
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        list(APPEND compile_flags -fcf-protection)
    endif()
    get_target_property(is_fuzzer ${target} BRAURSER_FUZZER)
    if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$" AND
       "${BRAURSER_SANITIZE}" STREQUAL "" AND NOT is_fuzzer)
        list(APPEND compile_flags -D_FORTIFY_SOURCE=3)
    endif()

    set(link_flags -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
    if(NOT "${BRAURSER_SANITIZE}" STREQUAL "")
        string(REPLACE ";" "," sanitizers "${BRAURSER_SANITIZE}")
        list(APPEND compile_flags "-fsanitize=${sanitizers}" -fno-omit-frame-pointer)
        list(APPEND link_flags "-fsanitize=${sanitizers}")
    endif()

    foreach(flag IN LISTS compile_flags)
        string(MAKE_C_IDENTIFIER "${flag}" flag_id)
        set(CMAKE_REQUIRED_LINK_OPTIONS "")
        if(flag MATCHES "^-fsanitize=")
            set(CMAKE_REQUIRED_LINK_OPTIONS "${flag}")
        endif()
        check_cxx_compiler_flag("${flag}" "BRAURSER_CXX_FLAG_${flag_id}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "")
        if(NOT BRAURSER_CXX_FLAG_${flag_id})
            message(FATAL_ERROR "Required C++ compiler flag is unsupported: ${flag}")
        endif()
    endforeach()
    foreach(flag IN LISTS link_flags)
        string(MAKE_C_IDENTIFIER "${flag}" flag_id)
        check_linker_flag(CXX "${flag}" "BRAURSER_LINK_FLAG_${flag_id}")
        if(NOT BRAURSER_LINK_FLAG_${flag_id})
            message(FATAL_ERROR "Required C++ linker flag is unsupported: ${flag}")
        endif()
    endforeach()

    target_compile_options(${target} PRIVATE ${compile_flags})
    target_link_options(${target} PRIVATE ${link_flags})
endfunction()
