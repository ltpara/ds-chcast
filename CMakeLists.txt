cmake_minimum_required(VERSION 3.24)

project(chast
    VERSION 0.1.0
    DESCRIPTION "Screen streaming stack modelled on RustDesk"
    LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "chast currently supports the Windows DXGI backend only; use MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(CHAST_BUILD_TESTS "Build the capture test tools" ON)

# Intel oneVPL. The static dispatcher is linked directly (VPL_SHARED=OFF is the
# config file's default, which is what D:/vpl_install_static ships); the
# runtime (libvpl.dll + the Intel driver's libmfxhw64.dll) is loaded by it at
# MFXLoad time. Point VPL_ROOT at the install when find_package cannot see it.
if(NOT DEFINED VPL_ROOT AND EXISTS "D:/vpl_install_static/lib/cmake/VPL")
    set(VPL_ROOT "D:/vpl_install_static" CACHE PATH "Intel oneVPL install prefix")
endif()
set(VPL_SHARED OFF CACHE BOOL "Use the shared instead of static oneVPL dispatcher")
find_package(VPL REQUIRED COMPONENTS dispatcher)

# Keep PDBs out of the source tree and give every target the same warning set.
add_compile_options(/W4 /permissive- /EHsc /utf-8 /Zc:preprocessor)

add_library(chast.dxgi STATIC
    src/win/com.cc
    src/capture/display.cc
    src/capture/rotation.cc
    src/capture/shader.cc
    src/capture/hdr_pass.cc
    src/capture/rotation_pass.cc
    src/capture/nv12_pass.cc
    src/capture/cursor.cc
    src/capture/dxgi_capturer.cc)

target_include_directories(chast.dxgi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.dxgi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# d3dcompiler is delay-loaded: shader compilation is the only thing that needs
# it, and the HDR pass is gated on D3DCompile being present at runtime, so a
# system without the DLL falls back instead of failing to load.
target_link_libraries(chast.dxgi PUBLIC
    d3d11
    dxgi
    user32
    gdi32
    ole32
    delayimp
    d3dcompiler)

target_link_options(chast.dxgi PUBLIC "/DELAYLOAD:d3dcompiler_47.dll")

# Hardware encoder. Kept out of chast.dxgi so the capture library has no
# dependency on the vendor runtime; it consumes what the capturer publishes.
add_library(chast.codec STATIC
    src/codec/vpl_encoder.cc)

target_include_directories(chast.codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(chast.codec PRIVATE ONEVPL_EXPERIMENTAL)
target_link_libraries(chast.codec PUBLIC chast.dxgi VPL::dispatcher)

if(CHAST_BUILD_TESTS)
    add_executable(chast-capture tools/capture.cc tools/crash_handler.cc)
    target_link_libraries(chast-capture PRIVATE chast.dxgi dbghelp)

    # Capture -> NV12 -> oneVPL encode, writing an elementary stream: the
    # end-to-end proof that no frame leaves the GPU on the way to the encoder.
    add_executable(chast-encode tools/encode.cc tools/crash_handler.cc)
    target_link_libraries(chast-encode PRIVATE chast.codec dbghelp)

    add_executable(chast-dump tools/dump.cc tools/crash_handler.cc)
    target_link_libraries(chast-dump PRIVATE chast.dxgi dbghelp)

    # Raw duplicate/acquire loop: isolates a driver or platform fault from the
    # capturer's own code when debugging.
    add_executable(chast-dup-loop tools/dup_loop.cc tools/crash_handler.cc)
    target_link_libraries(chast-dup-loop PRIVATE d3d11 dxgi dbghelp)

    # Pure-logic checks for the cursor helpers: no duplication, no GPU.
    add_executable(chast-cursor-test tests/cursor_test.cc)
    target_include_directories(chast-cursor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(chast-cursor-test PRIVATE chast.dxgi)

    enable_testing()
    add_test(NAME cursor COMMAND chast-cursor-test)
endif()
