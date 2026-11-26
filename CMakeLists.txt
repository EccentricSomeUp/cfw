# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

cmake_minimum_required(VERSION 3.25)

project(cfw
    VERSION 0.1.0
    DESCRIPTION "A security-focused, dependency-free C web framework"
    LANGUAGES C
)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(cmake/cfw_compile_options.cmake)

set(CFW_ABI_VERSION 1 CACHE STRING "Stable public ABI version")

option(CFW_BUILD_TESTING "Build the cfw test suite" ON)
option(CFW_BUILD_EXAMPLES "Build the cfw example programs" ON)
option(CFW_BUILD_SHARED "Build cfw as a shared library" OFF)
option(CFW_ENABLE_WERROR "Treat compiler warnings as errors" ON)
option(CFW_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(CFW_ENABLE_HARDENING "Enable available platform hardening flags" ON)

configure_file(
    cmake/cfw_config.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/include/cfw/cfw_config.h"
    @ONLY
)

set(CFW_SOURCES
    src/cfw/arena.c
    src/cfw/buffer.c
    src/cfw/checked_arithmetic.c
    src/cfw/cookie.c
    src/cfw/crypto.c
    src/cfw/fd.c
    src/cfw/dispatch.c
    src/cfw/fs.c
    src/cfw/http.c
    src/cfw/http1.c
    src/cfw/http1_conn.c
    src/cfw/log.c
    src/cfw/multipart.c
    src/cfw/platform.c
    src/cfw/private/net.c
    src/cfw/private/pollset.c
    src/cfw/query.c
    src/cfw/random.c
    src/cfw/route.c
    src/cfw/server.c
    src/cfw/span.c
    src/cfw/static.c
    src/cfw/status.c
    src/cfw/strview.c
    src/cfw/time.c
    src/cfw/uri.c
    src/cfw/version.c
    src/cfw/ws.c
)

if(CFW_BUILD_SHARED)
    add_library(cfw SHARED ${CFW_SOURCES})
    target_compile_definitions(cfw PRIVATE CFW_BUILDING_DLL)
    target_compile_definitions(cfw PUBLIC CFW_BUILD_SHARED)
else()
    add_library(cfw STATIC ${CFW_SOURCES})
endif()

set_target_properties(cfw PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(cfw
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

cfw_apply_strict_flags(cfw)
cfw_apply_hardening_flags(cfw)
cfw_apply_sanitizer_flags(cfw)

if(WIN32)
    target_link_libraries(cfw PRIVATE bcrypt)
endif()

if(CFW_BUILD_SHARED)
    set_target_properties(cfw PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
endif()

install(TARGETS cfw
    EXPORT cfwTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING
    PATTERN "*.h"
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/cfw/cfw_config.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cfw
)

install(EXPORT cfwTargets
    FILE cfwTargets.cmake
    NAMESPACE cfw::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cfw
)

configure_package_config_file(
    cmake/cfw-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cfwConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cfw
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cfwConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cfwConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cfwConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cfw
)

if(CFW_BUILD_EXAMPLES)
    add_executable(cfw_example_minimal_server examples/minimal_server.c)
    target_link_libraries(cfw_example_minimal_server PRIVATE cfw)
    cfw_apply_strict_flags(cfw_example_minimal_server)
    cfw_apply_hardening_flags(cfw_example_minimal_server)
    cfw_apply_sanitizer_flags(cfw_example_minimal_server)

    add_executable(cfw_example_ws_echo examples/ws_echo.c)
    target_link_libraries(cfw_example_ws_echo PRIVATE cfw)
    cfw_apply_strict_flags(cfw_example_ws_echo)
    cfw_apply_hardening_flags(cfw_example_ws_echo)
    cfw_apply_sanitizer_flags(cfw_example_ws_echo)
endif()

if(CFW_BUILD_TESTING)
    add_subdirectory(tests)
endif()
