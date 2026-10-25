cmake_minimum_required(VERSION 3.16)
project(hatari_c_example C)
find_program(MAKE_EXECUTABLE make REQUIRED)

# CLion indexes main.c through CMake; the actual Atari cross-build uses Make.
add_custom_target(atari ALL
    COMMAND ${MAKE_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR}
    SOURCES main.c)
