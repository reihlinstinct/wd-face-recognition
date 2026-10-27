cmake_minimum_required(VERSION 3.18)
project(wdf-demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Baseline x86-64 only: the PR4100's N3710 (Braswell) has SSE4.2, no AVX.
add_compile_options(-O2 -mno-avx -Wall -Wextra)

find_package(ncnn REQUIRED PATHS ${NCNN_INSTALL_DIR}/lib/cmake/ncnn NO_DEFAULT_PATH)
find_package(OpenMP REQUIRED)
find_path(JPEG_INCLUDE_DIR jpeglib.h REQUIRED)
find_library(JPEG_LIBRARY NAMES jpeg REQUIRED)

add_executable(wdf_demo src/main.cpp)
target_include_directories(wdf_demo PRIVATE ${JPEG_INCLUDE_DIR})
target_link_libraries(wdf_demo PRIVATE ncnn ${JPEG_LIBRARY} OpenMP::OpenMP_CXX)
