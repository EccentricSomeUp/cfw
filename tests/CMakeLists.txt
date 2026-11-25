# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

add_executable(cfw_unit_tests
    unit/test_main.c
    unit/test_status.c
    unit/test_version.c
)

target_include_directories(cfw_unit_tests PRIVATE unit)
target_link_libraries(cfw_unit_tests PRIVATE cfw)

cfw_apply_strict_flags(cfw_unit_tests)
cfw_apply_hardening_flags(cfw_unit_tests)
cfw_apply_sanitizer_flags(cfw_unit_tests)

add_test(NAME cfw_unit COMMAND cfw_unit_tests)
