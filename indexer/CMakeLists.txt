cmake_minimum_required(VERSION 3.18)
project(wdf-indexer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Baseline x86-64 only: the PR4100's N3710 (Braswell) has SSE4.2, no AVX.
add_compile_options(-O2 -mno-avx -Wall -Wextra)

# Pinned SQLite amalgamation: run indexer/get-sqlite.sh first.
add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party)

set(WDF_PIPELINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../pipeline/src)

add_executable(wdf_indexer src/main.cpp src/config.cpp src/db.cpp src/walk.cpp)
target_link_libraries(wdf_indexer PRIVATE sqlite3 pthread dl)

# Face-processing daemon (issue #40 PR 2): needs ncnn + libjpeg, so it is
# off by default - CI's plain-g++ indexer job and the NAS-less local build
# stay dependency-free.
option(WDF_FACES "Build wdf_faces (needs ncnn + libjpeg)" OFF)
if(WDF_FACES)
    find_package(ncnn REQUIRED PATHS ${NCNN_INSTALL_DIR}/lib/cmake/ncnn NO_DEFAULT_PATH)
    find_package(OpenMP REQUIRED)
    find_path(JPEG_INCLUDE_DIR jpeglib.h REQUIRED)
    find_library(JPEG_LIBRARY NAMES jpeg REQUIRED)
    add_executable(wdf_faces
        src/faces_main.cpp src/process.cpp src/config.cpp src/db.cpp
        src/cropreserve.cpp src/people.cpp
        ${WDF_PIPELINE_DIR}/cluster.cpp
        ${WDF_PIPELINE_DIR}/align.cpp ${WDF_PIPELINE_DIR}/detect.cpp
        ${WDF_PIPELINE_DIR}/jpegio.cpp)
    target_include_directories(wdf_faces PRIVATE ${WDF_PIPELINE_DIR} ${JPEG_INCLUDE_DIR})
    target_link_libraries(wdf_faces PRIVATE sqlite3 ncnn ${JPEG_LIBRARY} OpenMP::OpenMP_CXX pthread dl)
endif()

option(WDF_INDEXER_TESTS "Build the indexer unit tests" OFF)
if(WDF_INDEXER_TESTS)
    enable_testing()
    add_executable(wfi_test tests/indexer_test.cpp src/config.cpp src/db.cpp src/walk.cpp src/cropreserve.cpp
        src/people.cpp ${WDF_PIPELINE_DIR}/cluster.cpp)
    target_include_directories(wfi_test PRIVATE ${WDF_PIPELINE_DIR})
    target_link_libraries(wfi_test PRIVATE sqlite3 pthread dl)
    add_test(NAME indexer COMMAND wfi_test)
    # CLI dispatch test binary (issue #65): real faces_main with a stubbed
    # process stage, so no ncnn/libjpeg is needed.
    add_executable(wdf_faces_cli src/faces_main.cpp tests/process_stub.cpp
        src/config.cpp src/db.cpp src/walk.cpp src/cropreserve.cpp
        src/people.cpp ${WDF_PIPELINE_DIR}/cluster.cpp)
    target_include_directories(wdf_faces_cli PRIVATE src ${WDF_PIPELINE_DIR})
    target_link_libraries(wdf_faces_cli PRIVATE sqlite3 pthread dl)
endif()
