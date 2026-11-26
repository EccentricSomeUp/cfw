# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

add_executable(cfw_unit_tests
    ../src/cfw/private/net.c
    ../src/cfw/private/pollset.c
    unit/test_arena.c
    unit/test_buffer.c
    unit/test_checked_arithmetic.c
    unit/test_codec.c
    unit/test_cookie.c
    unit/test_crypto.c
    unit/test_dispatch.c
    unit/test_fd.c
    unit/test_fs.c
    unit/test_http.c
    unit/test_http1.c
    unit/test_http1_conn.c
    unit/test_http_fuzz.c
    unit/test_log.c
    unit/test_main.c
    unit/test_multipart.c
    unit/test_net.c
    unit/test_platform.c
    unit/test_pollset.c
    unit/test_query.c
    unit/test_random.c
    unit/test_route.c
    unit/test_server.c
    unit/test_signed.c
    unit/test_span.c
    unit/test_static.c
    unit/test_status.c
    unit/test_strview.c
    unit/test_uri.c
    unit/test_ws.c
    unit/test_time.c
    unit/test_version.c
)

target_include_directories(cfw_unit_tests PRIVATE
    unit
    ${CMAKE_SOURCE_DIR}/src/cfw
)
target_link_libraries(cfw_unit_tests PRIVATE cfw)
target_compile_definitions(cfw_unit_tests PRIVATE
    CFW_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus"
)

cfw_apply_strict_flags(cfw_unit_tests)
cfw_apply_hardening_flags(cfw_unit_tests)
cfw_apply_sanitizer_flags(cfw_unit_tests)

add_test(NAME cfw_unit COMMAND cfw_unit_tests)

add_executable(cfw_integration_tests
    ../src/cfw/private/net.c
    ../src/cfw/private/pollset.c
    integration/main.c
    integration/test_http_pipeline.c
)

target_include_directories(cfw_integration_tests PRIVATE
    unit
    ${CMAKE_SOURCE_DIR}/src/cfw
)
target_link_libraries(cfw_integration_tests PRIVATE cfw)

cfw_apply_strict_flags(cfw_integration_tests)
cfw_apply_hardening_flags(cfw_integration_tests)
cfw_apply_sanitizer_flags(cfw_integration_tests)

add_test(NAME cfw_integration COMMAND cfw_integration_tests)

add_executable(cfw_security_tests
    ../src/cfw/private/net.c
    ../src/cfw/private/pollset.c
    security/main.c
    security/test_smuggling.c
)

target_include_directories(cfw_security_tests PRIVATE
    unit
    ${CMAKE_SOURCE_DIR}/src/cfw
)
target_link_libraries(cfw_security_tests PRIVATE cfw)

cfw_apply_strict_flags(cfw_security_tests)
cfw_apply_hardening_flags(cfw_security_tests)
cfw_apply_sanitizer_flags(cfw_security_tests)

add_test(NAME cfw_security COMMAND cfw_security_tests)

# Poll-backend variant: compiles the pollset suite against the poll()
# fallback (CFW_POLLF_FORCE_POLL) so the non-epoll path is verified on
# Linux, where it would otherwise only run on macOS/BSD.
if(NOT WIN32)
    add_executable(cfw_pollset_poll_tests
        ../src/cfw/private/pollset.c
        unit/test_pollset.c
        unit/test_pollset_main.c
    )

    target_include_directories(cfw_pollset_poll_tests PRIVATE
        unit
        ${CMAKE_SOURCE_DIR}/src/cfw
    )
    target_link_libraries(cfw_pollset_poll_tests PRIVATE cfw)
    target_compile_definitions(cfw_pollset_poll_tests PRIVATE
        CFW_POLLF_FORCE_POLL=1
    )

    cfw_apply_strict_flags(cfw_pollset_poll_tests)
    cfw_apply_hardening_flags(cfw_pollset_poll_tests)
    cfw_apply_sanitizer_flags(cfw_pollset_poll_tests)

    add_test(NAME cfw_pollset_poll COMMAND cfw_pollset_poll_tests)
endif()

# C++ consumer smoke test: all public headers must compile as C++ and the
# exported symbols must link. Only built when a C++ compiler is available.
include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    add_executable(cfw_cpp_consumer cpp_consumer/main.cpp)
    target_link_libraries(cfw_cpp_consumer PRIVATE cfw)
    cfw_apply_strict_flags(cfw_cpp_consumer)
    cfw_apply_hardening_flags(cfw_cpp_consumer)
    cfw_apply_sanitizer_flags(cfw_cpp_consumer)
    add_test(NAME cfw_cpp_consumer COMMAND cfw_cpp_consumer)
endif()

# Install/package smoke test: installs into a scratch prefix, then configures,
# builds, and runs a consumer project against the installed package files.
# Skipped under the sanitizers preset: the installed static library is
# sanitizer-instrumented, and a plain consumer cannot link it without
# propagating sanitizer runtimes, which is out of scope for a packaging test.
if(NOT CFW_ENABLE_SANITIZERS)
    add_test(NAME cfw_install_smoke
        COMMAND ${CMAKE_COMMAND}
            -DCMAKE_COMMAND=${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DBINARY_DIR=${CMAKE_BINARY_DIR}
            -DPREFIX=${CMAKE_BINARY_DIR}/install-smoke
            -P ${CMAKE_CURRENT_SOURCE_DIR}/install_smoke.cmake
    )
endif()
