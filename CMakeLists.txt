cmake_minimum_required(VERSION 2.8)
project(fake86)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

add_definitions("-D_CRT_SECURE_NO_WARNINGS")

file(GLOB SOURCE_CPU src/80x86/*.h src/80x86/*.c)
add_library(lib_80x86 ${SOURCE_CPU})

file(GLOB SOURCE_F86 src/fake86/*.h src/fake86/*.c)
add_library(lib_fake86 ${SOURCE_F86})

file(GLOB SOURCE_FRONTEND src/frontend/*.h src/frontend/*.c)
add_executable(fake86 ${SOURCE_FRONTEND})

target_link_libraries(fake86 lib_fake86 lib_80x86 SDL.lib)