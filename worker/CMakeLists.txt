cmake_minimum_required(VERSION 3.18)
project(wdf-worker VERSION 0.1.0 LANGUAGES CXX)

option(WDF_STATIC "Link the worker fully statically (required for the NAS package)" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(wdf-worker src/main.cpp)
target_compile_options(wdf-worker PRIVATE -Wall -Wextra -Wpedantic)

if(WDF_STATIC)
    target_link_options(wdf-worker PRIVATE -static)
endif()
