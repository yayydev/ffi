cmake_minimum_required(VERSION 3.15)
project(ffi C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /O2 /GL /arch:AVX2 /DNDEBUG")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -march=native -flto -DNDEBUG")
endif()

file(GLOB SOURCES "../*.c")

set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ffi.c
)

add_executable(ffi ${SOURCES})
