# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

# TLS test certificates (ADR 0057): generated into the build tree at
# configure time so no private key material is ever committed. Requires
# the openssl CLI that ships with OpenSSL 3.x installations.
set(CFW_TEST_TLS_DIR "")
if(CFW_HAVE_TLS)
    # FindOpenSSL does not always locate the openssl CLI; fall back to
    # PATH. Certificate generation happens at configure time into the
    # build tree.
    if(NOT OPENSSL_EXECUTABLE)
        find_program(OPENSSL_EXECUTABLE openssl)
    endif()
    if(NOT OPENSSL_EXECUTABLE)
        message(FATAL_ERROR
            "CFW_ENABLE_TLS requires the openssl CLI to generate test "
            "certificates; install the OpenSSL tools package.")
    endif()
    set(CFW_TEST_TLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/tls-certs")
    file(MAKE_DIRECTORY "${CFW_TEST_TLS_DIR}")
    set(_cfw_tls_cert_cmds
        "req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=cfw-test-ca -keyout ${CFW_TEST_TLS_DIR}/ca.key -out ${CFW_TEST_TLS_DIR}/ca.crt"
        "req -newkey rsa:2048 -nodes -subj /CN=localhost -keyout ${CFW_TEST_TLS_DIR}/server.key -out ${CFW_TEST_TLS_DIR}/server.csr"
        "x509 -req -in ${CFW_TEST_TLS_DIR}/server.csr -CA ${CFW_TEST_TLS_DIR}/ca.crt -CAkey ${CFW_TEST_TLS_DIR}/ca.key -CAcreateserial -days 3650 -out ${CFW_TEST_TLS_DIR}/server.crt"
        "req -newkey rsa:2048 -nodes -subj /CN=cfw-test-client -keyout ${CFW_TEST_TLS_DIR}/client.key -out ${CFW_TEST_TLS_DIR}/client.csr"
        "x509 -req -in ${CFW_TEST_TLS_DIR}/client.csr -CA ${CFW_TEST_TLS_DIR}/ca.crt -CAkey ${CFW_TEST_TLS_DIR}/ca.key -CAcreateserial -days 3650 -out ${CFW_TEST_TLS_DIR}/client.crt"
    )
    foreach(_cfw_tls_cert_cmd IN LISTS _cfw_tls_cert_cmds)
        separate_arguments(_cfw_tls_cert_args NATIVE_COMMAND
            "${_cfw_tls_cert_cmd}")
        execute_process(
            COMMAND "${OPENSSL_EXECUTABLE}" ${_cfw_tls_cert_args}
            RESULT_VARIABLE _cfw_tls_cert_result
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _cfw_tls_cert_result EQUAL 0)
            message(FATAL_ERROR
                "TLS test certificate generation failed: "
                "${OPENSSL_EXECUTABLE} ${_cfw_tls_cert_cmd}")
        endif()
    endforeach()
endif()

add_executable(cfw_unit_tests
    ../src/cfw/pollset.c
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
    fuzz/fuzz_support.c
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
    unit/test_sync.c
    unit/test_thread.c
    unit/test_tls.c
    unit/test_uri.c
    unit/test_ws.c
    unit/test_time.c
    unit/test_version.c
)

target_include_directories(cfw_unit_tests PRIVATE
    unit
    fuzz
    ${CMAKE_SOURCE_DIR}/src/cfw
)
target_link_libraries(cfw_unit_tests PRIVATE cfw)
target_compile_definitions(cfw_unit_tests PRIVATE
    CFW_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus"
)
if(CFW_HAVE_TLS)
    target_compile_definitions(cfw_unit_tests PRIVATE
        CFW_TEST_TLS_DIR="${CFW_TEST_TLS_DIR}"
    )
endif()

cfw_apply_strict_flags(cfw_unit_tests)
cfw_apply_hardening_flags(cfw_unit_tests)
cfw_apply_sanitizer_flags(cfw_unit_tests)

add_test(NAME cfw_unit COMMAND cfw_unit_tests)

add_executable(cfw_integration_tests
    ../src/cfw/pollset.c
    integration/main.c
    integration/test_http_client.c
    integration/test_http_pipeline.c
    integration/test_ws_client.c
)

target_include_directories(cfw_integration_tests PRIVATE
    unit
    ${CMAKE_SOURCE_DIR}/src/cfw
)
target_link_libraries(cfw_integration_tests PRIVATE cfw)
if(CFW_HAVE_TLS)
    target_compile_definitions(cfw_integration_tests PRIVATE
        CFW_TEST_TLS_DIR="${CFW_TEST_TLS_DIR}"
    )
endif()

cfw_apply_strict_flags(cfw_integration_tests)
cfw_apply_hardening_flags(cfw_integration_tests)
cfw_apply_sanitizer_flags(cfw_integration_tests)

add_test(NAME cfw_integration COMMAND cfw_integration_tests)

add_executable(cfw_security_tests
    ../src/cfw/pollset.c
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
# fallback (CFW_POLLF_BACKEND=poll) so the non-epoll/kqueue path is
# verified on every POSIX CI host.
if(NOT WIN32)
    add_executable(cfw_pollset_poll_tests
        ../src/cfw/pollset.c
        unit/test_pollset.c
        unit/test_pollset_main.c
    )

    target_include_directories(cfw_pollset_poll_tests PRIVATE
        unit
        ${CMAKE_SOURCE_DIR}/src/cfw
    )
    target_link_libraries(cfw_pollset_poll_tests PRIVATE cfw)
    target_compile_definitions(cfw_pollset_poll_tests PRIVATE
        CFW_POLLF_BACKEND=poll
    )

    cfw_apply_strict_flags(cfw_pollset_poll_tests)
    cfw_apply_hardening_flags(cfw_pollset_poll_tests)
    cfw_apply_sanitizer_flags(cfw_pollset_poll_tests)

    add_test(NAME cfw_pollset_poll COMMAND cfw_pollset_poll_tests)
endif()

# Backend compile fixtures: these object targets verify that each backend
# selection still compiles when forced, without requiring the host to provide
# every platform's runtime headers. kqueue uses a minimal stub header on
# hosts without <sys/event.h>; real kqueue coverage remains on macOS/BSD CI.
if(CFW_HAVE_EPOLL)
    add_library(cfw_pollset_epoll_compile OBJECT
        ../src/cfw/pollset.c
    )
    target_include_directories(cfw_pollset_epoll_compile PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
    )
    target_compile_definitions(cfw_pollset_epoll_compile PRIVATE
        CFW_POLLF_BACKEND=epoll
    )
    cfw_apply_strict_flags(cfw_pollset_epoll_compile)
endif()

if(NOT WIN32)
    add_library(cfw_pollset_poll_compile OBJECT
        ../src/cfw/pollset.c
    )
    target_include_directories(cfw_pollset_poll_compile PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
    )
    target_compile_definitions(cfw_pollset_poll_compile PRIVATE
        CFW_POLLF_BACKEND=poll
    )
    cfw_apply_strict_flags(cfw_pollset_poll_compile)
endif()

if(NOT WIN32 AND NOT CFW_HAVE_KQUEUE)
    add_library(cfw_pollset_kqueue_compile OBJECT
        ../src/cfw/pollset.c
    )
    target_include_directories(cfw_pollset_kqueue_compile PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/include
    )
    target_compile_definitions(cfw_pollset_kqueue_compile PRIVATE
        CFW_POLLF_BACKEND=kqueue
    )
    cfw_apply_strict_flags(cfw_pollset_kqueue_compile)
endif()

add_library(cfw_pollset_none_compile OBJECT
    ../src/cfw/pollset.c
)
target_include_directories(cfw_pollset_none_compile PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include
)
target_compile_definitions(cfw_pollset_none_compile PRIVATE
    CFW_POLLF_BACKEND=none
)
cfw_apply_strict_flags(cfw_pollset_none_compile)

# Loopback server benchmark: manual measurement tool (not a ctest).
# POSIX only — it uses the private socket layer like the unit tests.
if(NOT WIN32)
    add_executable(cfw_bench
        ../src/cfw/pollset.c
        bench/bench_server.c
    )

    target_include_directories(cfw_bench PRIVATE
        unit
        ${CMAKE_SOURCE_DIR}/src/cfw
    )
    target_link_libraries(cfw_bench PRIVATE cfw)

    cfw_apply_strict_flags(cfw_bench)
    cfw_apply_hardening_flags(cfw_bench)
    cfw_apply_sanitizer_flags(cfw_bench)
endif()

# Windows runtime smoke test (ADR 0072/0074): the POSIX unit/integration
# suites cannot execute on Windows, so this target exercises the public
# socket/pollset/server surface over loopback using only portable API.
if(WIN32)
    add_executable(cfw_windows_smoke windows_smoke.c)
    target_include_directories(cfw_windows_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src/cfw
    )
    target_link_libraries(cfw_windows_smoke PRIVATE cfw)
    if(CFW_HAVE_TLS)
        target_compile_definitions(cfw_windows_smoke PRIVATE
            CFW_TEST_TLS_DIR="${CFW_TEST_TLS_DIR}"
        )
    endif()
    cfw_apply_strict_flags(cfw_windows_smoke)
    cfw_apply_hardening_flags(cfw_windows_smoke)
    cfw_apply_sanitizer_flags(cfw_windows_smoke)
    add_test(NAME cfw_windows_smoke COMMAND cfw_windows_smoke)
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

# Coverage-guided fuzz targets (ADR 0077). Off by default: requires a
# fuzzing-capable toolchain for the interesting mode. With Clang the
# targets link libFuzzer (-fsanitize=fuzzer); other compilers build the
# standalone corpus driver (fuzz_driver.c) so the same LLVMFuzzerTestOneInput
# entry points still run under ASan/UBSan for corpus replay and manual
# fuzzing.
if(CFW_ENABLE_FUZZING)
    set(_cfw_fuzz_targets http uri query multipart head_reader ws_proto crypto http1_conn)
    set(_cfw_fuzz_libfuzzer FALSE)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(_cfw_fuzz_libfuzzer TRUE)
    endif()

    foreach(_cfw_fuzz_t IN LISTS _cfw_fuzz_targets)
        set(_cfw_fuzz_srcs fuzz/fuzz_${_cfw_fuzz_t}.c)
        if(_cfw_fuzz_t STREQUAL "http")
            list(APPEND _cfw_fuzz_srcs fuzz/fuzz_support.c)
        endif()
        if(_cfw_fuzz_t STREQUAL "ws_proto")
            list(APPEND _cfw_fuzz_srcs ../src/cfw/private/ws_proto.c)
        endif()
        if(NOT _cfw_fuzz_libfuzzer)
            list(APPEND _cfw_fuzz_srcs fuzz/fuzz_driver.c)
        endif()

        add_executable(cfw_fuzz_${_cfw_fuzz_t} ${_cfw_fuzz_srcs})
        target_include_directories(cfw_fuzz_${_cfw_fuzz_t} PRIVATE
            fuzz
            unit
            ${CMAKE_SOURCE_DIR}/src/cfw
        )
        target_link_libraries(cfw_fuzz_${_cfw_fuzz_t} PRIVATE cfw)

        if(_cfw_fuzz_libfuzzer)
            target_compile_options(cfw_fuzz_${_cfw_fuzz_t} PRIVATE
                -fsanitize=fuzzer-no-link,address,undefined)
            target_link_options(cfw_fuzz_${_cfw_fuzz_t} PRIVATE
                -fsanitize=fuzzer,address,undefined)
        elseif(MSVC)
            target_compile_options(cfw_fuzz_${_cfw_fuzz_t} PRIVATE
                /fsanitize=address)
        else()
            target_compile_options(cfw_fuzz_${_cfw_fuzz_t} PRIVATE
                -fsanitize=address,undefined)
            target_link_options(cfw_fuzz_${_cfw_fuzz_t} PRIVATE
                -fsanitize=address,undefined)
        endif()
    endforeach()
endif()
