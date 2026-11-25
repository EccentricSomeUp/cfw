# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

function(cfw_apply_strict_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:preprocessor
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wredundant-decls
            -Wundef
            -Wcast-align
            -Wwrite-strings
            -Wpointer-arith
            -Winit-self
            -Wformat=2
        )
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
