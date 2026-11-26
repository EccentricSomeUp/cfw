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

file(REMOVE_RECURSE "${PREFIX}")
file(REMOVE_RECURSE "${BINARY_DIR}/install-smoke-build")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BINARY_DIR}"
            --prefix "${PREFIX}"
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
            -DCMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "consumer configure failed with ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}"
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "consumer build failed with ${build_result}")
endif()

execute_process(
    COMMAND "${consumer_build}/cfw_install_smoke_consumer"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "consumer run failed with ${run_result}")
endif()

message(STATUS "install smoke test passed")
