cmake_minimum_required(VERSION 3.10)
project(paralax-captcha C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(paralax-captcha
    src/main.c
)

target_include_directories(paralax-captcha PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(paralax-captcha PRIVATE -Wall -Wextra)
elseif(CMAKE_C_COMPILER_ID MATCHES "MSVC")
    target_compile_options(paralax-captcha PRIVATE /W4)
endif()
