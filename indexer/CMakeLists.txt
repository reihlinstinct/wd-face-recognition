cmake_minimum_required(VERSION 3.18)
project(wdf-indexer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Baseline x86-64 only: the PR4100's N3710 (Braswell) has SSE4.2, no AVX.
add_compile_options(-O2 -mno-avx -Wall -Wextra)

# Pinned SQLite amalgamation: run indexer/get-sqlite.sh first.
add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party)

add_executable(wdf_indexer src/main.cpp src/config.cpp src/db.cpp src/walk.cpp)
target_link_libraries(wdf_indexer PRIVATE sqlite3 pthread dl)

option(WDF_INDEXER_TESTS "Build the indexer unit tests" OFF)
if(WDF_INDEXER_TESTS)
    enable_testing()
    add_executable(wfi_test tests/indexer_test.cpp src/config.cpp src/db.cpp src/walk.cpp)
    target_link_libraries(wfi_test PRIVATE sqlite3 pthread dl)
    add_test(NAME indexer COMMAND wfi_test)
endif()
