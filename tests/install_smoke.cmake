# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Install/package smoke test, driven by `cmake -P` from CTest.
# Installs cfw into a scratch prefix, then builds and runs a consumer
# project that locates the package via find_package(cfw).

if(NOT DEFINED CMAKE_COMMAND OR NOT DEFINED SOURCE_DIR OR
   NOT DEFINED BINARY_DIR OR NOT DEFINED PREFIX)
    message(FATAL_ERROR "install_smoke.cmake requires CMAKE_COMMAND, "
                        "SOURCE_DIR, BINARY_DIR, and PREFIX")
endif()

# EXPECTED_CONFIG is the configuration under test ($<CONFIG> from
# add_test); multi-config generators need it for --install/--build.
set(install_config_args "")
set(consumer_config "Release")
if(DEFINED EXPECTED_CONFIG AND NOT EXPECTED_CONFIG STREQUAL "")
    list(APPEND install_config_args --config "${EXPECTED_CONFIG}")
    set(consumer_config "${EXPECTED_CONFIG}")
endif()

file(REMOVE_RECURSE "${PREFIX}")
file(REMOVE_RECURSE "${BINARY_DIR}/install-smoke-build")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BINARY_DIR}"
            --prefix "${PREFIX}" ${install_config_args}
    RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed with ${install_result}")
endif()

foreach(required
        "${PREFIX}/include/cfw/cfw.h"
        "${PREFIX}/include/cfw/cfw_config.h")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "missing installed file: ${required}")
    endif()
endforeach()

file(GLOB config_files "${PREFIX}/*/cmake/cfw/cfwConfig.cmake")
list(LENGTH config_files config_count)
if(NOT config_count EQUAL 1)
    message(FATAL_ERROR "expected exactly one installed cfwConfig.cmake, "
                        "found ${config_count}")
endif()

set(consumer_build "${BINARY_DIR}/install-smoke-build")
set(ENV{CMAKE_PREFIX_PATH} "${PREFIX}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${SOURCE_DIR}/tests/install_smoke/consumer"
            -B "${consumer_build}"
            -DCMAKE_BUILD_TYPE=${consumer_config}
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "consumer configure failed with ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}"
            ${install_config_args}
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "consumer build failed with ${build_result}")
endif()

# Multi-config generators place the binary under <config>/ and use an
# .exe suffix on Windows; single-config builds put it at the top level.
set(consumer_exe "${consumer_build}/cfw_install_smoke_consumer")
if(DEFINED EXPECTED_CONFIG AND NOT EXPECTED_CONFIG STREQUAL "")
    set(consumer_exe_cfg
        "${consumer_build}/${EXPECTED_CONFIG}/cfw_install_smoke_consumer")
    if(EXISTS "${consumer_exe_cfg}.exe")
        set(consumer_exe "${consumer_exe_cfg}.exe")
    elseif(EXISTS "${consumer_exe_cfg}")
        set(consumer_exe "${consumer_exe_cfg}")
    endif()
endif()
if(EXISTS "${consumer_exe}.exe")
    set(consumer_exe "${consumer_exe}.exe")
endif()
if(NOT EXISTS "${consumer_exe}")
    message(FATAL_ERROR "consumer binary not found: ${consumer_exe}")
endif()

execute_process(
    COMMAND "${consumer_exe}"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "consumer run failed with ${run_result}")
endif()

message(STATUS "install smoke test passed")
