cmake_minimum_required(VERSION 3.24)

# Honour MSQUIC_ROOT when locating the MsQuic config package.
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

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

# FFmpeg static build for hardware decoding. D3D11VA lives in libavutil and
# libavcodec supplies the H.264/HEVC decoders; vcpkg's x64-windows-static
# triplet ships both, so the receiver needs no FFmpeg DLLs at run time.
if(NOT DEFINED FFMPEG_ROOT AND EXISTS "D:/vcpkg/installed/x64-windows-static/lib/avcodec.lib")
    set(FFMPEG_ROOT "D:/vcpkg/installed/x64-windows-static" CACHE PATH "FFmpeg static install prefix")
endif()
if(NOT DEFINED FFMPEG_ROOT OR NOT EXISTS "${FFMPEG_ROOT}/lib/avcodec.lib")
    message(FATAL_ERROR "FFmpeg static libraries not found; set FFMPEG_ROOT to an install prefix with lib/avcodec.lib")
endif()
add_library(chast.ffmpeg INTERFACE)
target_include_directories(chast.ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
target_link_libraries(chast.ffmpeg INTERFACE
    "${FFMPEG_ROOT}/lib/avcodec.lib"
    "${FFMPEG_ROOT}/lib/avutil.lib"
    "${FFMPEG_ROOT}/lib/swresample.lib"
    mfuuid
    strmiids
    bcrypt)

# Hardware encoder. Kept out of chast.dxgi so the capture library has no
# dependency on the vendor runtime; it consumes what the capturer publishes.
# The decoder is FFmpeg D3D11VA; the oneVPL decoder is shelved in favour of it.
add_library(chast.codec STATIC
    src/codec/vpl_encoder.cc
    src/codec/ffmpeg_decoder.cc)

target_include_directories(chast.codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(chast.codec PRIVATE ONEVPL_EXPERIMENTAL)
target_link_libraries(chast.codec PUBLIC chast.dxgi VPL::dispatcher chast.ffmpeg)

# MsQuic. The vcpkg install ships a config package under share/msquic; point
# MSQUIC_ROOT at the install prefix when find_package cannot see it.
if(NOT DEFINED MSQUIC_ROOT AND EXISTS "D:/vcpkg/installed/x64-windows/share/msquic")
    set(MSQUIC_ROOT "D:/vcpkg/installed/x64-windows" CACHE PATH "MsQuic install prefix")
endif()
if(DEFINED MSQUIC_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${MSQUIC_ROOT}")
endif()
find_package(msquic CONFIG REQUIRED)

# The Flutter Visual Studio generator adds a Profile configuration that
# neither the VPL package nor vcpkg's MsQuic exports. Map Profile onto the
# release binaries instead of modifying the installed packages.
if("Profile" IN_LIST CMAKE_CONFIGURATION_TYPES)
    foreach(_imported_target IN ITEMS VPL::dispatcher msquic)
        if(TARGET ${_imported_target})
            get_target_property(_release_location ${_imported_target} IMPORTED_LOCATION_RELEASE)
            if(_release_location)
                set_target_properties(${_imported_target} PROPERTIES
                    IMPORTED_LOCATION_PROFILE "${_release_location}")
            endif()
            get_target_property(_release_implib ${_imported_target} IMPORTED_IMPLIB_RELEASE)
            if(_release_implib)
                set_target_properties(${_imported_target} PROPERTIES
                    IMPORTED_IMPLIB_PROFILE "${_release_implib}")
            endif()
        endif()
    endforeach()
endif()

# chast wire protocol and MsQuic session. No DXGI or VPL dependency, so the
# receiver side links this without the capture stack.
add_library(chast.net STATIC
    src/net/protocol.cc
    src/net/quic.cc)

target_include_directories(chast.net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# QuicAddrFromString() lives in msquic_winuser.h and calls ntdll's
# RtlIpv*StringToAddressEx, which the imported target does not declare.
target_link_libraries(chast.net PUBLIC msquic crypt32 ncrypt ws2_32 ntdll)

if(CHAST_BUILD_TESTS)
    add_executable(chast-capture tools/capture.cc tools/crash_handler.cc)
    target_link_libraries(chast-capture PRIVATE chast.dxgi dbghelp)

    # Capture -> NV12 -> oneVPL encode, writing an elementary stream: the
    # end-to-end proof that no frame leaves the GPU on the way to the encoder.
    add_executable(chast-encode tools/encode.cc tools/crash_handler.cc)
    target_link_libraries(chast-encode PRIVATE chast.codec dbghelp)

    add_executable(chast-dump tools/dump.cc tools/crash_handler.cc)
    target_link_libraries(chast-dump PRIVATE chast.dxgi dbghelp)

    # Elementary-stream decoder harness: proves the receiver's VPL path
    # without a network or a capture device.
    add_executable(chast-decode tools/decode.cc tools/crash_handler.cc)
    target_link_libraries(chast-decode PRIVATE chast.codec dbghelp)

    # Capture -> encode -> QUIC -> decode. serve/connect run the two ends as
    # separate processes; verify closes the loop in one process and checks the
    # decoded content against an offline replay of the sent stream.
    add_executable(chast-quic tools/quic.cc tools/crash_handler.cc)
    target_link_libraries(chast-quic PRIVATE chast.codec chast.net dbghelp)
    add_custom_command(TARGET chast-quic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:chast-quic>
        VERBATIM)

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

    # Localhost loopback: needs an interactive desktop like the other capture
    # tools, so it is not part of the pure-logic set.
    add_test(NAME quic_loopback
        COMMAND chast-quic verify --port 45433 --seconds 4 --request-keyframe-ms 1200 --quiet)
    set_tests_properties(quic_loopback PROPERTIES TIMEOUT 120)
endif()
