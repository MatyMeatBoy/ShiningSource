# Native side of the Android app: SDL3 and the host library SDL's activity loads (libmain.so).
# The game itself is not built here: it is the image tools/build_arm64.py (arm64-v8a) or tools/build_win64.py
# (x86_64, the emulator) produce, shipped as an asset of the APK. tools/build_android.py drives all of it.
cmake_minimum_required(VERSION 3.22)
project(ShiningSourceAndroid C)
if(NOT ANDROID OR (NOT ANDROID_ABI STREQUAL "arm64-v8a" AND NOT ANDROID_ABI STREQUAL "x86_64"))
    message(FATAL_ERROR "Android ABIs: arm64-v8a and x86_64 (the port is 64-bit)")
endif()
if(NOT SDL_SOURCE_DIR)
    message(FATAL_ERROR "Set SDL_SOURCE_DIR to an SDL3 source tree")
endif()
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/SDL")

add_library(main SHARED
    "${ROOT}/runtime/host64/host_linux.c"
    "${ROOT}/runtime/win32/plat_sdl.c"
    "${ROOT}/runtime/plat/net_udp.c")
target_include_directories(main PRIVATE "${ROOT}/runtime/include")
target_compile_options(main PRIVATE -O2 -Wall)
target_link_libraries(main PRIVATE SDL3::SDL3 log android m)
