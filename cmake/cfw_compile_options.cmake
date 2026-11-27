# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

function(cfw_apply_strict_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:preprocessor
            # MSVC only enables C11 library features such as <stdatomic.h>
            # in /std:c11 mode; the default C dialect leaves them disabled.
            # C11 atomics additionally require the experimental switch.
            $<$<COMPILE_LANGUAGE:C>:/std:c11>
            $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wundef
            -Wcast-align
            -Wwrite-strings
            -Wpointer-arith
            -Winit-self
            -Wformat=2
        )

        # These warnings are only valid for C; GCC/Clang diagnose them on
        # C++ translation units, which breaks -Werror builds.
        get_target_property(_cfw_sources ${target} SOURCES)
        if(NOT _cfw_sources MATCHES "\\.(cpp|cc|cxx|CXX)$")
            target_compile_options(${target} PRIVATE
                -Wstrict-prototypes
                -Wmissing-prototypes
                -Wredundant-decls
            )
        endif()
    endif()

    if(CFW_ENABLE_WERROR)
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(cfw_apply_hardening_flags target)
    if(NOT CFW_ENABLE_HARDENING)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /GS /guard:cf)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -fstack-protector-strong
        )

        if(UNIX AND NOT APPLE)
            target_link_options(${target} PRIVATE
                -Wl,-z,relro
                -Wl,-z,now
            )
        endif()
    endif()
endfunction()

function(cfw_apply_sanitizer_flags target)
    if(CFW_ENABLE_TSAN)
        if(MSVC)
            message(FATAL_ERROR
                "CFW_ENABLE_TSAN is not supported on MSVC; "
                "ThreadSanitizer requires Clang or GCC")
        endif()
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -fsanitize=thread
                -fno-omit-frame-pointer
            )
            target_link_options(${target} PRIVATE
                -fsanitize=thread
            )
        endif()
    endif()

    if(NOT CFW_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /fsanitize=address)
    else()
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    endif()
endfunction()
