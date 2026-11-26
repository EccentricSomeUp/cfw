# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

cmake_minimum_required(VERSION 3.25)

project(cfw_install_smoke_consumer LANGUAGES C)

find_package(cfw 0.1 REQUIRED)

add_executable(cfw_install_smoke_consumer main.c)
target_link_libraries(cfw_install_smoke_consumer PRIVATE cfw::cfw)
