cmake_minimum_required(VERSION 3.18)
project(wdf-demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Baseline x86-64 only: the PR4100's N3710 (Braswell) has SSE4.2, no AVX.
add_compile_options(-O2 -mno-avx -Wall -Wextra)

# The apkg packages a fully static wdf_demo (no library assumptions on the NAS).
option(WDF_DEMO_STATIC "Link wdf_demo fully statically for the NAS package" OFF)
if(WDF_DEMO_STATIC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

find_package(ncnn REQUIRED PATHS ${NCNN_INSTALL_DIR}/lib/cmake/ncnn NO_DEFAULT_PATH)
find_package(OpenMP REQUIRED)
find_path(JPEG_INCLUDE_DIR jpeglib.h REQUIRED)
find_library(JPEG_LIBRARY NAMES jpeg REQUIRED)

add_executable(wdf_demo src/main.cpp src/scan.cpp)
target_include_directories(wdf_demo PRIVATE ${JPEG_INCLUDE_DIR})
target_link_libraries(wdf_demo PRIVATE ncnn ${JPEG_LIBRARY} OpenMP::OpenMP_CXX)

if(WDF_DEMO_STATIC)
    # Fully static glibc link: the gcc driver appends its implicit -lgomp
    # AFTER all explicit libraries, so libgomp.a's dlopen/dlsym refs would
    # come after -ldl and stay undefined. Name gomp explicitly so dl and
    # pthread land after it; the driver's duplicate -lgomp is harmless.
    target_link_libraries(wdf_demo PRIVATE gomp dl pthread)
    target_link_options(wdf_demo PRIVATE -static)
endif()

# Unit tests for the scanner need no ncnn (issue #24). CI compiles the same
# two files directly with g++; this target is for local ctest runs.
option(WDF_DEMO_TESTS "Build the demo scan unit tests" OFF)
if(WDF_DEMO_TESTS)
    enable_testing()
    add_executable(wdf_scan_test tests/scan_test.cpp src/scan.cpp)
    target_include_directories(wdf_scan_test PRIVATE src)
    add_test(NAME scan COMMAND wdf_scan_test)
endif()
