cmake_minimum_required(VERSION 3.24)

project(chast_gpu_sync_bench LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "This benchmark models the Windows D3D11 sink; use MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(chast-gpu-sync-bench main.cc)
target_compile_options(chast-gpu-sync-bench PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(chast-gpu-sync-bench PRIVATE d3d11 dxgi d3d10 winmm)
