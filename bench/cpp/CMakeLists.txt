cmake_minimum_required(VERSION 3.18)
project(wdf-bench-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Baseline x86-64 only: the PR4100's N3710 (Braswell) has SSE4.2, no AVX.
add_compile_options(-O2 -mno-avx -Wall -Wextra)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_path(JPEG_INCLUDE_DIR jpeglib.h REQUIRED)
find_library(JPEG_LIBRARY NAMES jpeg REQUIRED)
find_path(EXIV2_INCLUDE_DIR exiv2/exiv2.hpp REQUIRED)
find_library(EXIV2_LIBRARY NAMES exiv2 REQUIRED)
find_library(SQLITE3_LIBRARY NAMES sqlite3 REQUIRED)

# cpp-httplib: single header, MIT. Pinned download (see CMake FetchContent below
# is avoided on purpose; bench/cpp/third_party/httplib.h is fetched by the
# build script with a pinned URL + sha256, like mksapkg-OS5).
find_path(HTTPLIB_INCLUDE_DIR httplib.h PATHS ${CMAKE_CURRENT_SOURCE_DIR}/third_party REQUIRED)
find_path(JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

add_executable(bench_cpp src/main.cpp)
target_include_directories(bench_cpp PRIVATE
    ${JPEG_INCLUDE_DIR} ${EXIV2_INCLUDE_DIR} ${HTTPLIB_INCLUDE_DIR} ${JSON_INCLUDE_DIR})
target_link_libraries(bench_cpp PRIVATE
    OpenSSL::Crypto ${JPEG_LIBRARY} ${EXIV2_LIBRARY} ${SQLITE3_LIBRARY} Threads::Threads)
