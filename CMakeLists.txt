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

# Status text and the retry policy, shared by every layer. The COM helpers stay
# in chast.dxgi so the audio codec can format an Error without the capture
# stack.
add_library(chast.base STATIC
    src/win/error.cc)
target_include_directories(chast.base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Password hashing, TOTP, DPAPI protection and the viewer's device key. CNG
# supplies PBKDF2/HMAC/ECDSA, crypt32 the data-protection API; neither has a
# dependency on the capture or network stacks.
add_library(chast.security STATIC
    src/security/security.cc
    src/win/user_context.cc)
target_include_directories(chast.security PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(chast.security PUBLIC chast.base bcrypt crypt32)

add_library(chast.dxgi STATIC
    src/win/com.cc
    src/win/png_writer.cc
    src/capture/display.cc
    src/capture/display_mode.cc
    src/capture/rotation.cc
    src/capture/shader.cc
    src/capture/hdr_pass.cc
    src/capture/rotation_pass.cc
    src/capture/nv12_pass.cc
    src/capture/cursor.cc
    src/capture/dxgi_capturer.cc
    src/capture/gdi_capturer.cc
    src/capture/shared_capturer.cc
    src/capture/merged_capturer.cc)

target_include_directories(chast.dxgi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.dxgi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# d3dcompiler is delay-loaded: shader compilation is the only thing that needs
# it, and the HDR pass is gated on D3DCompile being present at runtime, so a
# system without the DLL falls back instead of failing to load.
target_link_libraries(chast.dxgi PUBLIC
    chast.base
    d3d11
    dxgi
    user32
    gdi32
    ole32
    windowscodecs
    delayimp
    d3dcompiler)

target_link_options(chast.dxgi PUBLIC "/DELAYLOAD:d3dcompiler_47.dll")

# Standalone Opus from vcpkg. FFmpeg's static build also registers an Opus
# decoder, so its registry still needs these symbols, but chast calls
# opus_encode/opus_decode itself through Opus::opus instead of the libopus
# wrapper inside FFmpeg.
if(NOT DEFINED OPUS_ROOT AND EXISTS "D:/vcpkg/installed/x64-windows-static/share/opus")
    set(OPUS_ROOT "D:/vcpkg/installed/x64-windows-static" CACHE PATH "Opus install prefix")
endif()
if(DEFINED OPUS_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${OPUS_ROOT}")
endif()
find_package(Opus CONFIG REQUIRED)

# Zstandard for the v66 compressed CFIL block format. The static triplet
# matches Opus, so the library is linked in and no DLL ships beside the host
# or the viewer.
if(NOT DEFINED ZSTD_ROOT AND EXISTS "D:/vcpkg/installed/x64-windows-static/share/zstd")
    set(ZSTD_ROOT "D:/vcpkg/installed/x64-windows-static" CACHE PATH "zstd install prefix")
endif()
if(DEFINED ZSTD_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${ZSTD_ROOT}")
endif()
find_package(zstd CONFIG REQUIRED)

# VP9 software encoding/decoding and direct full-chroma pixel conversion.
if(NOT DEFINED VPX_ROOT AND EXISTS "D:/vcpkg/installed/x64-windows-static-md/include/vpx/vpx_codec.h")
    set(VPX_ROOT "D:/vcpkg/installed/x64-windows-static-md" CACHE PATH "libvpx and libyuv install prefix")
endif()
if(DEFINED VPX_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${VPX_ROOT}")
endif()
find_package(unofficial-libvpx CONFIG REQUIRED)
find_package(libyuv CONFIG REQUIRED)
set_target_properties(unofficial::libvpx::libvpx yuv PROPERTIES MAP_IMPORTED_CONFIG_PROFILE Release)

# Ed25519 verification for v62 access tickets. vcpkg's static OpenSSL build
# supplies libcrypto; hosts pin the issuer public keys out of band, so the
# ticket signature never depends on the deployment host or link keys.
if(NOT DEFINED OPENSSL_ROOT_DIR AND EXISTS "D:/vcpkg/installed/x64-windows-static-md/lib/libcrypto.lib")
    set(OPENSSL_ROOT_DIR "D:/vcpkg/installed/x64-windows-static-md" CACHE PATH "OpenSSL install prefix")
endif()
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# Access-ticket v1 encoding and Ed25519 proofs. The host verifies tickets and
# the viewer signs possession proofs, so this sits below both products: it has
# no Windows dependencies beyond libcrypto, and neither side duplicates the
# byte format.
add_library(chast.access STATIC src/host/access_ticket.cc)
target_include_directories(chast.access PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(chast.access PUBLIC OpenSSL::Crypto)

# FFmpeg static build for hardware decoding. D3D11VA lives in libavutil and
# libavcodec supplies the H.264/HEVC decoders; vcpkg's x64-windows-static
# triplet ships both, so the receiver needs no FFmpeg DLLs at run time.
if(NOT DEFINED FFMPEG_ROOT AND EXISTS "D:/vcpkg/installed/x64-windows-static/lib/avcodec.lib")
    set(FFMPEG_ROOT "D:/vcpkg/installed/x64-windows-static" CACHE PATH "FFmpeg static install prefix")
endif()
if(NOT DEFINED FFMPEG_ROOT OR NOT EXISTS "${FFMPEG_ROOT}/lib/avcodec.lib")
    message(FATAL_ERROR "FFmpeg static libraries not found; set FFMPEG_ROOT to an install prefix with lib/avcodec.lib")
endif()
add_library(chast.ffmpeg STATIC src/codec/ffmpeg_compat.cc)
target_include_directories(chast.ffmpeg PUBLIC "${FFMPEG_ROOT}/include")
target_link_libraries(chast.ffmpeg PUBLIC
    "${FFMPEG_ROOT}/lib/avcodec.lib"
    "${FFMPEG_ROOT}/lib/avutil.lib"
    "${FFMPEG_ROOT}/lib/swresample.lib"
    # avcodec's codec registry pulls the Opus objects, which are the only
    # external codec in this static build.
    Opus::opus
    VPL::dispatcher
    mfuuid
    strmiids
    bcrypt)

# Hardware encoder. Kept out of chast.dxgi so the capture library has no
# dependency on the vendor runtime; it consumes what the capturer publishes.
# The decoder is FFmpeg D3D11VA; the oneVPL decoder is shelved in favour of it.
add_library(chast.codec STATIC
    src/codec/encoder_factory.cc
    src/codec/ffmpeg_encoder.cc
    src/codec/vpx_encoder.cc
    src/codec/vpx_decoder.cc
    src/codec/vpl_encoder.cc
    src/codec/ffmpeg_decoder.cc)

target_include_directories(chast.codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(chast.codec PRIVATE ONEVPL_EXPERIMENTAL)
target_link_libraries(chast.codec PUBLIC chast.dxgi VPL::dispatcher chast.ffmpeg
    unofficial::libvpx::libvpx yuv)

# Audio: Opus on the wire, WASAPI loopback capture and render (host) plus
# microphone capture (viewer). AAC-LC stays for older recordings and tests.
# Kept out of chast.codec so a build that only needs video does not link the
# audio endpoints; it has no capture-stack dependency beyond the headers
# protocol.h pulls in.
#
# SpeexDSP (BSD-3-Clause) supplies the echo canceller and preprocessor. vcpkg
# ships no CMake package for it, so the install prefix is located directly.
if(NOT DEFINED SPEEXDSP_ROOT AND EXISTS "D:/vcpkg/installed/x64-windows-static/include/speex/speex_echo.h")
    set(SPEEXDSP_ROOT "D:/vcpkg/installed/x64-windows-static" CACHE PATH "SpeexDSP install prefix")
endif()
if(NOT DEFINED SPEEXDSP_ROOT OR NOT EXISTS "${SPEEXDSP_ROOT}/include/speex/speex_echo.h")
    message(FATAL_ERROR "SpeexDSP not found; set SPEEXDSP_ROOT to an install prefix with include/speex and lib/speexdsp.lib")
endif()
add_library(SpeexDSP::speexdsp STATIC IMPORTED)
set_target_properties(SpeexDSP::speexdsp PROPERTIES
    IMPORTED_LOCATION "${SPEEXDSP_ROOT}/lib/speexdsp.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${SPEEXDSP_ROOT}/include")

add_library(chast.audio STATIC
    src/audio/aac_codec.cc
    src/audio/opus_codec.cc
    src/audio/audio_dsp.cc
    src/audio/wasapi_capture.cc
    src/audio/wasapi_mic.cc
    src/audio/wasapi_player.cc)

target_include_directories(chast.audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chast.audio PUBLIC chast.base chast.ffmpeg Opus::opus SpeexDSP::speexdsp ole32 propsys)

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

# Multi-format clipboard content, its CCLB container codec and the Win32
# clipboard backend. Shared by the host and the viewer; neither knows about
# capture or network.
add_library(chast.clipboard STATIC
    src/clipboard/clipboard.cc
    src/clipboard/clipboard_worker.cc
    src/clipboard/clipboard_virtual.cc
    src/clipboard/clipboard_local.cc
    src/clipboard/clipboard_oledata.cc
    src/clipboard/clipboard_listener.cc
    src/clipboard/clipboard_win.cc)
target_include_directories(chast.clipboard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(chast.clipboard PUBLIC chast.base chast.security user32 shell32 ole32 uuid shlwapi zstd::libzstd)

# Receive-side file handling: name sanitizing, resumable .part files, digest
# verification. The receiver side only; the viewer hashes what it sends through
# the same library.
add_library(chast.transfer STATIC
    src/clipboard/file_transfer.cc)
target_include_directories(chast.transfer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(chast.transfer PUBLIC chast.base chast.security shell32)

# chast wire protocol, the MsQuic session and server-side input injection. No
# DXGI or VPL dependency, so the receiver side links this without the capture
# stack.
add_library(chast.net STATIC
    src/net/clipboard_channel.cc
    src/net/carrier.cc
    src/net/identity.cc
    src/net/port_forward_client.cc
    src/net/protocol.cc
    src/net/quic.cc
    src/win/desktop.cc
    src/win/input.cc)

target_include_directories(chast.net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# QuicAddrFromString() lives in msquic_winuser.h and calls ntdll's
# RtlIpv*StringToAddressEx, which the imported target does not declare.
# user32 supplies SendInput/GetSystemMetrics for the injector; wtsapi32 the
# session lock state behind the desktop tracker; shcore the per-monitor DPI
# declaration that keeps those APIs in physical pixels.
target_link_libraries(chast.net PUBLIC chast.base chast.security chast.clipboard msquic crypt32 ncrypt ws2_32 ntdll user32 wtsapi32 shcore zstd::libzstd)

# Host product code shared by the tray application and the command-line
# server: settings persistence and the capture/encode/serve loop. It sits on
# top of both chast.codec and chast.net so neither of those has to know about
# the other.
add_library(chast.host STATIC
    src/capture/camera.cc
    src/capture/camera_capturer.cc
    src/host/audit_spool.cc
    src/host/camera_relay.cc
    src/host/display_privacy.cc
    src/host/desktop_stream.cc
    src/host/stream_recorder.cc
    src/host/recording_disk.cc
    src/host/video_qos.cc
    src/host/shared_loopback.cc
    src/host/host_security.cc
    src/host/host_settings.cc
    src/host/host_server.cc
    src/host/port_forward_relay.cc
    src/host/terminal_relay.cc
    src/host/virtual_display.cc
    src/win/display_privacy.cc
    src/win/privacy.cc
    src/win/power_request.cc
    src/win/sas.cc
    src/win/show_cursor.cc
    src/win/cursor_hint_paint.cc
    src/win/wallpaper.cc)

target_include_directories(chast.host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(chast.host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_link_libraries(chast.host PUBLIC "${FFMPEG_ROOT}/lib/avformat.lib" secur32)
# Media Foundation supplies camera enumeration and capture; mf holds the
# device-source helpers and mfuuid the format GUIDs. SetupAPI and CfgMgr
# identify virtual display adapters and their monitor children.
target_link_libraries(chast.host PUBLIC chast.codec chast.net chast.audio chast.clipboard chast.transfer chast.access shell32 ole32 mf mfplat mfreadwrite mfuuid setupapi cfgmgr32 advapi32 wintrust bcrypt winmm OpenSSL::Crypto crypt32 ws2_32)

# Windows integration shared by the tray and the service: firewall/autostart
# helpers, the Go tunnel child process and SCM control. Kept apart from
# chast.host so the command-line tools do not pull in Win32 process management
# just for the settings.
add_library(chast.host.tray STATIC
    host/i18n.cc
    host/i18n_en.cc
    host/platform.cc
    host/elevation_handoff.cc
    host/tunnel.cc
    host/service.cc
    host/service_control.cc
    host/service_manager_startup.cc
    host/windows_sessions.cc
    host/service_settings.cc
    host/service_log.cc)
target_include_directories(chast.host.tray PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host)
target_link_libraries(chast.host.tray PUBLIC chast.host shell32 advapi32 wtsapi32 userenv)

# Notification-area host application. It is product code, so it is built even
# when the test tools are switched off.
add_executable(chast-host WIN32 host/main.cc host/connections.cc host/service_manager.cc)
target_link_libraries(chast-host PRIVATE chast.host.tray comctl32 comdlg32)

# Crash-safe physical-display restoration helper. It receives only inherited
# kernel handles from the host and has no network or user configuration path.
add_executable(chast-display-guard host/display_guard_main.cc)
target_include_directories(chast-display-guard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chast-display-guard PRIVATE chast.host)
add_custom_command(TARGET chast-display-guard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:chast-display-guard> $<TARGET_FILE_DIR:chast-host>
    VERBATIM)
add_custom_command(TARGET chast-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:chast-host>
    VERBATIM)

# The tray spawns the same Go agent the viewer uses. go build is incremental,
# so keeping the target on every build picks up agent changes without a
# reconfigure; the executable lands next to chast-host.exe.
find_program(GO_EXECUTABLE go)
if(GO_EXECUTABLE)
    set(CHAST_TUNNEL_OUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/chast-tunnel.exe")
    add_custom_target(chast_tunnel_binary ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
        COMMAND ${GO_EXECUTABLE} build -trimpath -o "${CHAST_TUNNEL_OUT}"
                ./cmd/chast-tunnel
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/server"
        BYPRODUCTS "${CHAST_TUNNEL_OUT}"
        COMMENT "Building chast-tunnel.exe"
        VERBATIM)
    add_dependencies(chast-host chast_tunnel_binary)
else()
    message(WARNING
        "Go was not found; the tray cannot register with a rendezvous service "
        "until chast-tunnel.exe is placed next to chast-host.exe.")
endif()

# Windows service host. The SCM entry point and the session worker share one
# executable: the service launches itself with --worker, so an upgrade only has
# to replace one file.
add_executable(chast-service host/service_main.cc host/worker.cc)
target_link_libraries(chast-service PRIVATE chast.host.tray)
add_custom_command(TARGET chast-service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:chast-service>
    VERBATIM)
if(GO_EXECUTABLE)
    add_dependencies(chast-service chast_tunnel_binary)
endif()

if(CHAST_BUILD_TESTS)
    add_executable(chast-capture tools/capture.cc tools/crash_handler.cc)
    target_link_libraries(chast-capture PRIVATE chast.dxgi dbghelp)

    # Capture -> NV12 -> hardware or software encode -> elementary stream.
    add_executable(chast-encode tools/encode.cc tools/crash_handler.cc)
    target_link_libraries(chast-encode PRIVATE chast.codec dbghelp)

    add_executable(chast-dump tools/dump.cc tools/crash_handler.cc)
    target_link_libraries(chast-dump PRIVATE chast.dxgi dbghelp)

    # Elementary-stream decoder harness: proves the receiver's VPL path
    # without a network or a capture device.
    add_executable(chast-decode tools/decode.cc tools/crash_handler.cc)
    target_link_libraries(chast-decode PRIVATE chast.codec dbghelp)

    # Userspace weak-link relay for the QoS acceptance runs: a UDP hop with
    # delay, jitter, loss and a bandwidth bottleneck that the transport sees
    # as a real bad path. Independent of the chast libraries on purpose.
    add_executable(chast-netem tools/netem.cc)
    target_link_libraries(chast-netem PRIVATE ws2_32 winmm)

    # Capture -> encode -> QUIC -> decode. serve/connect run the two ends as
    # separate processes; verify closes the loop in one process and checks the
    # decoded content against an offline replay of the sent stream.
    add_executable(chast-quic tools/quic.cc tools/crash_handler.cc)
    # avformat is only needed for --record: the tool muxes the received stream
    # into an MP4 with the container writer, which the receive pipeline itself
    # never touches. Its TLS object resolves against SChannel.
    target_link_libraries(chast-quic PRIVATE chast.codec chast.net chast.host dbghelp
        "${FFMPEG_ROOT}/lib/avformat.lib" secur32)
    add_custom_command(TARGET chast-quic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:chast-quic>
        VERBATIM)

    # Headless viewer-engine acceptance: the Flutter shell only polls
    # ViewerStatus, so the real engine and a real HostServer run in one
    # process here to exercise the shared-folder walk over loopback QUIC.
    add_executable(chast-viewer-probe tools/viewer_probe.cc
        app/windows/viewer/viewer_engine.cc app/windows/viewer/display_video_receiver.cc)
    target_include_directories(chast-viewer-probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/app/windows/viewer
        ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    target_compile_definitions(chast-viewer-probe PRIVATE NOMINMAX)
    # avformat only backs the engine's local recording, as in the runner.
    target_link_libraries(chast-viewer-probe PRIVATE chast.host d3d11 dxgi
        "${FFMPEG_ROOT}/lib/avformat.lib" secur32 imm32)
    add_custom_command(TARGET chast-viewer-probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:chast-viewer-probe>
        VERBATIM)

    # Raw duplicate/acquire loop: isolates a driver or platform fault from the
    # capturer's own code when debugging.
    add_executable(chast-dup-loop tools/dup_loop.cc tools/crash_handler.cc)
    target_link_libraries(chast-dup-loop PRIVATE d3d11 dxgi dbghelp)

    # Pure-logic checks for the cursor helpers: no duplication, no GPU.
    add_executable(chast-cursor-test tests/cursor_test.cc)
    target_include_directories(chast-cursor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(chast-cursor-test PRIVATE chast.dxgi)

    # Shared duplication readers on one output: independent GPU textures and
    # NV12 scales, detection of a shared rebuild, and survival while the
    # merged compositor or another reader comes and goes. Skips without a
    # capturable desktop.
    add_executable(chast-shared-capturer-test tests/shared_capturer_test.cc)
    target_link_libraries(chast-shared-capturer-test PRIVATE chast.dxgi)

    # GPU color and geometry stages against known textures: HDR scRGB ->
    # sRGB, video-processor rotation and the BGRA -> NV12 downscale. Runs on
    # the real adapter and skips stages the device cannot provide.
    add_executable(chast-capture-transform-test tests/capture_transform_test.cc)
    target_include_directories(chast-capture-transform-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(chast-capture-transform-test PRIVATE chast.dxgi)

    # PNG writing for screenshots: signature, decoded pixels and alpha forcing
    # through WIC. No GPU, no network.
    add_executable(chast-png-writer-test tests/png_writer_test.cc)
    target_link_libraries(chast-png-writer-test PRIVATE chast.dxgi ole32 windowscodecs)

    # The viewer's sprite installer: validation, the scaled-handle cache and
    # release. Builds handles from a screen DC only, so it needs no desktop.
    add_executable(chast-remote-cursor-test tests/remote_cursor_test.cc
                   app/windows/viewer/remote_cursor.cc)
    target_include_directories(chast-remote-cursor-test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app/windows/viewer)
    target_link_libraries(chast-remote-cursor-test PRIVATE chast.dxgi)

    # The host counterpart of the sprite: the click ripple, the viewer colour
    # and label, and the premultiplied raster. Pure maths, no window.
    add_executable(chast-cursor-hint-test tests/cursor_hint_paint_test.cc
                   src/win/cursor_hint_paint.cc)
    target_include_directories(chast-cursor-hint-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # Pure-logic checks for the wire format and the HID -> scan-code table.
    add_executable(chast-protocol-test tests/protocol_test.cc)
    target_link_libraries(chast-protocol-test PRIVATE chast.net)

    add_executable(chast-keyboard-test tests/keyboard_test.cc
        app/windows/viewer/keyboard_capture.cc app/windows/viewer/local_text_input.cc)
    target_include_directories(chast-keyboard-test PRIVATE app/windows/viewer)
    target_link_libraries(chast-keyboard-test PRIVATE chast.net imm32)

    # Deterministic mutation coverage for every control parser, the stream
    # header decoders and the three reassembly parsers. Seeds are real maker
    # output; truncation, bit flips, insertions and random 1 MiB payloads must
    # only ever make a parser reject, never crash or over-allocate.
    add_executable(chast-protocol-fuzz-test tests/protocol_fuzz_test.cc)
    target_link_libraries(chast-protocol-fuzz-test PRIVATE chast.net)

    # The input-desktop tracker against the live session. Needs an interactive
    # desktop; the secure-desktop path cannot be produced from a test.
    add_executable(chast-desktop-test tests/desktop_test.cc)
    target_link_libraries(chast-desktop-test PRIVATE chast.net)

    # Cryptographic primitives against published vectors, plus a DPAPI and
    # device-key round trip in the user's own profile.
    add_executable(chast-security-test tests/security_test.cc)
    target_link_libraries(chast-security-test PRIVATE chast.security)

    # Settings file round-trip: no GPU, no network, no certificate store.
    add_executable(chast-host-settings-test tests/host_settings_test.cc)
    target_link_libraries(chast-host-settings-test PRIVATE chast.host.tray)

    # JSONL records for the service and worker: timestamp shape, escaping,
    # invalid-UTF-8 replacement, rotation and concurrent appends.
    add_executable(chast-service-log-test tests/service_log_test.cc)
    target_link_libraries(chast-service-log-test PRIVATE chast.host.tray)

    # Multi-viewer host over loopback QUIC: concurrent admission, the
    # connection cap, per-session audit records and the control lease. The
    # concurrency cases use file-transfer sessions, so no GPU is needed; the
    # lease case skips itself when no interactive desktop exists.
    add_executable(chast-host-multi-test tests/host_multi_test.cc)
    target_link_libraries(chast-host-multi-test PRIVATE chast.host)
    add_executable(chast-host-parity-test tests/host_parity_test.cc)
    target_link_libraries(chast-host-parity-test PRIVATE chast.host.tray)
    add_executable(chast-desktop-stream-test tests/desktop_stream_test.cc)
    target_link_libraries(chast-desktop-stream-test PRIVATE chast.host)
    add_executable(chast-recording-disk-test tests/recording_disk_test.cc)
    target_link_libraries(chast-recording-disk-test PRIVATE chast.host)
    add_executable(chast-display-subscription-test tests/display_subscription_test.cc)
    target_link_libraries(chast-display-subscription-test PRIVATE chast.net chast.access)
    add_executable(chast-display-video-test tests/display_video_receiver_test.cc
        app/windows/viewer/display_video_receiver.cc app/windows/viewer/viewer_engine.cc)
    target_include_directories(chast-display-video-test PRIVATE tools)
    target_compile_definitions(chast-display-video-test PRIVATE NOMINMAX)
    target_link_libraries(chast-display-video-test PRIVATE chast.host d3d11 dxgi
        "${FFMPEG_ROOT}/lib/avformat.lib" secur32)
    add_executable(chast-video-qos-test tests/video_qos_test.cc src/host/video_qos.cc)
    target_include_directories(chast-video-qos-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # Host voice call: request/accept, Opus uplink and permission revocation
    # against a synthetic capture. Needs a desktop session, so it skips itself
    # when no interactive desktop exists.
    add_executable(chast-host-voice-test tests/host_voice_test.cc)
    target_link_libraries(chast-host-voice-test PRIVATE chast.host)

    # Host controls: lock, restart and block-input requests over loopback QUIC,
    # the permission and settings gates, the idempotent block toggle, and the
    # automatic release on revocation and session end. A synthetic backend
    # stands in for ExitWindowsEx/LockWorkStation/BlockInput. Needs a desktop
    # session, so it skips itself when no interactive desktop exists.
    add_executable(chast-host-control-test tests/host_control_test.cc)
    target_link_libraries(chast-host-control-test PRIVATE chast.host)

    add_executable(chast-clipboard-async-test tests/clipboard_async_test.cc
        app/windows/viewer/viewer_engine.cc app/windows/viewer/display_video_receiver.cc)
    target_include_directories(chast-clipboard-async-test PRIVATE app/windows/viewer tools)
    target_compile_definitions(chast-clipboard-async-test PRIVATE NOMINMAX)
    target_link_libraries(chast-clipboard-async-test PRIVATE chast.host d3d11 dxgi
        "${FFMPEG_ROOT}/lib/avformat.lib" secur32)
    add_custom_command(TARGET chast-clipboard-async-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:chast-clipboard-async-test>)

    # The shared clipboard listener: real WM_CLIPBOARDUPDATE on a private
    # window station, multiple subscribers, generation and unsubscribe. Skips
    # itself when the process cannot own a private window station.
    add_executable(chast-clipboard-listener-test tests/clipboard_listener_test.cc)
    target_link_libraries(chast-clipboard-listener-test PRIVATE chast.clipboard)
    add_executable(chast-clipboard-listener-startup-test tests/clipboard_listener_startup_test.cc)
    target_include_directories(chast-clipboard-listener-startup-test PRIVATE include)
    target_link_libraries(chast-clipboard-listener-startup-test PRIVATE user32)

    # OLE virtual file sources: descriptor parsing plus delayed reads from an
    # in-process IDataObject, including a source whose streams cannot seek.
    add_executable(chast-clipboard-oledata-test tests/clipboard_oledata_test.cc)
    target_compile_definitions(chast-clipboard-oledata-test PRIVATE NOMINMAX)
    target_link_libraries(chast-clipboard-oledata-test PRIVATE chast.net)
    add_custom_command(TARGET chast-clipboard-oledata-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:msquic> $<TARGET_FILE_DIR:chast-clipboard-oledata-test>)

    # OS login: the password request over loopback QUIC, the permission and
    # logon-state gates, the exact text handed to a synthetic injector, and the
    # transport-level rejection of empty or multi-line passwords. Needs a
    # desktop session, so it skips itself when no interactive desktop exists.
    add_executable(chast-host-login-test tests/host_login_test.cc)
    target_link_libraries(chast-host-login-test PRIVATE chast.host)

    # Block input over the v38 host-action channel: the handshake consent, the
    # idempotent toggle and the automatic release when the input or block-input
    # permission is revoked. A synthetic backend stands in for BlockInput.
    # Needs a desktop session, so it skips itself when none exists.
    add_executable(chast-block-input-test tests/block_input_test.cc)
    target_link_libraries(chast-block-input-test PRIVATE chast.host)

    # Workstation lock after the last session: the settings switch, live
    # cancellation, a failing platform call and the shutdown exemption, in both
    # the single-viewer and the shared host. A synthetic backend stands in for
    # LockWorkStation. Needs a desktop session, so it skips itself when none
    # exists.
    add_executable(chast-lock-after-session-test tests/lock_after_session_test.cc)
    target_link_libraries(chast-lock-after-session-test PRIVATE chast.host)

    # Two desktop viewers on one shared host: view-only admission, the
    # follower's privacy and host-action refusals, focused chat and gesture
    # gating, and the remaining viewer continuing after the publisher leaves.
    # Needs a desktop session, so it skips itself when none exists.
    add_executable(chast-multi-session-test tests/multi_session_test.cc)
    target_link_libraries(chast-multi-session-test PRIVATE chast.host)

    # Explicit opt-in hardware acceptance for virtual display privacy and
    # management. Never registered with CTest: it disconnects physical outputs
    # temporarily and needs an installed driver.
    add_executable(chast-display-privacy-live tests/display_privacy_live.cc)
    target_include_directories(chast-display-privacy-live PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(chast-display-privacy-live PRIVATE chast.host)
    add_dependencies(chast-display-privacy-live chast-display-guard)

    # Virtual display detection, classification and mode choice. The live
    # probe is read-only and runs without a driver.
    add_executable(chast-virtual-display-test tests/virtual_display_test.cc)
    target_link_libraries(chast-virtual-display-test PRIVATE chast.host)

    # Display-privacy lifecycle boundaries and synthetic CCD topology, plus
    # the recovery lease the guard and the host share. No driver or network.
    add_executable(chast-display-privacy-test tests/display_privacy_test.cc)
    target_include_directories(chast-display-privacy-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(chast-display-privacy-test PRIVATE chast.host)

    # Authentication policy: modes, lockout, temporary password, 2FA,
    # trusted devices, allow list and permission intersection.
    add_executable(chast-host-security-test tests/host_security_test.cc)
    target_link_libraries(chast-host-security-test PRIVATE chast.host)

    # Access-ticket v1: the frozen golden vector, strict decoding, issuer and
    # host binding, the time window and the bounded replay cache. No network.
    add_executable(chast-access-ticket-test tests/access_ticket_test.cc)
    target_link_libraries(chast-access-ticket-test PRIVATE chast.host)

    # v62 managed admission over loopback QUIC: the host challenge, the Hello
    # ticket and possession proof, and the refusals for a missing ticket or a
    # foreign signing key. File-transfer sessions, so no GPU or desktop.
    add_executable(chast-managed-access-test tests/managed_access_test.cc)
    target_link_libraries(chast-managed-access-test PRIVATE chast.host)

    # v62 host session lease: a ticket-admitted file session is closed by the
    # host at the signed expiry, on the monotonic deadline, without traffic
    # extending it. Loopback HostServer, no GPU.
    add_executable(chast-host-ticket-lease-test tests/host_ticket_lease_test.cc)
    target_link_libraries(chast-host-ticket-lease-test PRIVATE chast.host)

    # AAC encode/decode round trip: no device, no network.
    add_executable(chast-audio-codec-test tests/audio_codec_test.cc)
    target_link_libraries(chast-audio-codec-test PRIVATE chast.audio)

    # Echo cancellation and noise suppression against synthetic far/near
    # signals: no device, no network, deterministic.
    add_executable(chast-audio-dsp-test tests/audio_dsp_test.cc)
    target_link_libraries(chast-audio-dsp-test PRIVATE chast.audio)

    # Adaptive playout lead for arrival-anchored audio: windowed slack,
    # hysteresis, the raise ceiling and the give-back. No device.
    add_executable(chast-audio-playout-test tests/audio_playout_test.cc)
    target_link_libraries(chast-audio-playout-test PRIVATE chast.audio)

    # Receive-side file handling: name sanitizing, resumable .part files and
    # digest verification, all inside a temporary directory.
    add_executable(chast-file-transfer-test tests/file_transfer_test.cc)
    target_link_libraries(chast-file-transfer-test PRIVATE chast.transfer)

    # Tunnel event parsing, argument construction and child lifecycle. The
    # lifecycle case relaunches this executable as a fake agent.
    add_executable(chast-host-tunnel-test tests/host_tunnel_test.cc)
    target_link_libraries(chast-host-tunnel-test PRIVATE chast.host.tray)

    # Tray language selection: the environment override, the viewer's shared
    # ui.json and the English table. No window is created.
    add_executable(chast-host-i18n-test tests/host_i18n_test.cc)
    target_link_libraries(chast-host-i18n-test PRIVATE chast.host.tray)

    # Raw TCP and WebSocket carrier framing against a loopback UDP echo,
    # including the RFC 6455 handshake, bad frames and the connection limit.
    add_executable(chast-carrier-test tests/carrier_test.cc)
    target_link_libraries(chast-carrier-test PRIVATE chast.net)

    # FFmpeg's input-buffer contract: a network access unit has no
    # AV_INPUT_BUFFER_PADDING_SIZE tail, so the decoder must copy it into a
    # padded packet. The fixture sits flush against a PROTECT_NONE page, which
    # turns an overread into a crash instead of silent heap damage. The access
    # unit is deliberately truncated, so a decode error is expected.
    add_executable(chast-decoder-padding-test tests/decoder_padding_test.cc)
    target_link_libraries(chast-decoder-padding-test PRIVATE chast.codec)

    # The recording container: the viewer muxes the access units it received
    # into MP4 without re-encoding. Reopens the files with libavformat and
    # checks zero-based timing, the key-frame flags and the rollover after a
    # picture-size change.
    add_executable(chast-mp4-recorder-test tests/mp4_recorder_test.cc)
    target_include_directories(chast-mp4-recorder-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    # The static avformat object files pull its TLS, socket and RTSP helpers
    # even though this test only uses the MP4 demuxer.
    target_link_libraries(chast-mp4-recorder-test PRIVATE chast.host
        "${FFMPEG_ROOT}/lib/avformat.lib" secur32 crypt32 ws2_32 bcrypt ncrypt)

    enable_testing()
    add_test(NAME cursor COMMAND chast-cursor-test)
    add_test(NAME shared_capture COMMAND chast-shared-capturer-test)
    add_test(NAME capture_transform COMMAND chast-capture-transform-test)
    add_test(NAME png_writer COMMAND chast-png-writer-test)
    add_test(NAME remote_cursor COMMAND chast-remote-cursor-test)
    add_test(NAME cursor_hint COMMAND chast-cursor-hint-test)
    add_test(NAME protocol COMMAND chast-protocol-test)
    add_test(NAME keyboard COMMAND chast-keyboard-test)
    add_test(NAME protocol_fuzz COMMAND chast-protocol-fuzz-test)
    set_tests_properties(protocol_fuzz PROPERTIES TIMEOUT 300)
    add_test(NAME desktop COMMAND chast-desktop-test)
    add_test(NAME security COMMAND chast-security-test)
    add_test(NAME host_settings COMMAND chast-host-settings-test)
    add_test(NAME service_log COMMAND chast-service-log-test)
    add_test(NAME host_multi COMMAND chast-host-multi-test)
    set_tests_properties(host_multi PROPERTIES TIMEOUT 180)
    add_test(NAME host_parity COMMAND chast-host-parity-test)
    add_test(NAME desktop_stream COMMAND chast-desktop-stream-test)
    add_test(NAME shared_capturer COMMAND chast-shared-capturer-test)
    set_tests_properties(shared_capturer PROPERTIES TIMEOUT 60)
    add_test(NAME recording_disk COMMAND chast-recording-disk-test)
    set_tests_properties(recording_disk PROPERTIES TIMEOUT 30)
    add_test(NAME display_subscriptions COMMAND chast-display-subscription-test)
    add_test(NAME display_video COMMAND chast-display-video-test)
    add_test(NAME viewer_display_streams COMMAND chast-display-video-test --engine)
    set_tests_properties(viewer_display_streams PROPERTIES TIMEOUT 60)
    set_tests_properties(display_video PROPERTIES TIMEOUT 30)
    set_tests_properties(display_subscriptions PROPERTIES TIMEOUT 30)
    add_test(NAME video_qos COMMAND chast-video-qos-test)
    set_tests_properties(desktop_stream PROPERTIES TIMEOUT 30)
    set_tests_properties(host_parity PROPERTIES TIMEOUT 240)
    add_test(NAME host_voice COMMAND chast-host-voice-test)
    set_tests_properties(host_voice PROPERTIES TIMEOUT 180)
    add_test(NAME host_control COMMAND chast-host-control-test)
    set_tests_properties(host_control PROPERTIES TIMEOUT 180)
    add_test(NAME clipboard_async COMMAND chast-clipboard-async-test)
    set_tests_properties(clipboard_async PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_test(NAME clipboard_listener COMMAND chast-clipboard-listener-test)
    set_tests_properties(clipboard_listener PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    add_test(NAME clipboard_listener_startup COMMAND chast-clipboard-listener-startup-test)
    set_tests_properties(clipboard_listener_startup PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77)
    add_test(NAME clipboard_oledata COMMAND chast-clipboard-oledata-test)
    set_tests_properties(clipboard_oledata PROPERTIES TIMEOUT 60)
    add_test(NAME host_login COMMAND chast-host-login-test)
    set_tests_properties(host_login PROPERTIES TIMEOUT 180)
    add_test(NAME block_input COMMAND chast-block-input-test)
    set_tests_properties(block_input PROPERTIES TIMEOUT 180)
    add_test(NAME lock_after_session COMMAND chast-lock-after-session-test)
    set_tests_properties(lock_after_session PROPERTIES TIMEOUT 240)
    add_test(NAME multi_session COMMAND chast-multi-session-test)
    set_tests_properties(multi_session PROPERTIES TIMEOUT 180)
    add_test(NAME virtual_display COMMAND chast-virtual-display-test)
    add_test(NAME display_privacy COMMAND chast-display-privacy-test)
    add_test(NAME host_security COMMAND chast-host-security-test)
    add_test(NAME access_ticket COMMAND chast-access-ticket-test)
    add_test(NAME managed_access COMMAND chast-managed-access-test)
    add_test(NAME host_ticket_lease COMMAND chast-host-ticket-lease-test)
    set_tests_properties(host_ticket_lease PROPERTIES TIMEOUT 120)
    add_test(NAME host_tunnel COMMAND chast-host-tunnel-test)
    add_test(NAME host_i18n COMMAND chast-host-i18n-test)
    add_test(NAME audio COMMAND chast-audio-codec-test)
    add_test(NAME audio_dsp COMMAND chast-audio-dsp-test)
    add_test(NAME audio_playout COMMAND chast-audio-playout-test)
    add_test(NAME file_transfer COMMAND chast-file-transfer-test)
    add_test(NAME carrier COMMAND chast-carrier-test)
    add_test(NAME decoder_padding COMMAND chast-decoder-padding-test)
    # Raw-stream tooling must not guess Annex B framing for VP9.
    add_test(NAME decode_rejects_vp9
        COMMAND ${CMAKE_COMMAND}
                -DDECODE=$<TARGET_FILE:chast-decode>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tools/annexb.h
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/decode_rejects_vp9.cmake)
    add_test(NAME mp4_recorder COMMAND chast-mp4-recorder-test)

    # Localhost loopback: needs an interactive desktop like the other capture
    # tools, so it is not part of the pure-logic set.
    add_test(NAME quic_loopback
        COMMAND chast-quic verify --port 45433 --seconds 4 --request-keyframe-ms 1200
                --file-upload --file-download --privacy --quiet)
    set_tests_properties(quic_loopback PROPERTIES TIMEOUT 120)
    # The preference probe switches codec families mid-session, so it runs on
    # its own port and the plain loopback above keeps the offline replay check.
    # Port forwarding: the host dials a real loopback TCP echo endpoint and the
    # viewer's bytes travel QUIC -> host -> TCP -> echo and back.
    add_test(NAME quic_port_forward
        COMMAND chast-quic verify --port 45503 --port-forward --quiet)
    set_tests_properties(quic_port_forward PROPERTIES TIMEOUT 120)
    add_test(NAME quic_port_forward_denied
        COMMAND chast-quic verify --port 45504 --port-forward-denied --quiet)
    set_tests_properties(quic_port_forward_denied PROPERTIES TIMEOUT 120)
    # The viewer-side listener: a real local TCP client reaches the host's TCP
    # endpoint through the viewer's 127.0.0.1 listener and the CPFW streams.
    add_test(NAME quic_port_forward_local
        COMMAND chast-quic verify --port 45505 --port-forward-local --quiet)
    set_tests_properties(quic_port_forward_local PROPERTIES TIMEOUT 120)
    # Remote terminal: the in-process host starts a real shell under ConPTY,
    # the viewer types into it, and the console's own output must come back
    # before the host reports a clean exit.
    add_test(NAME quic_terminal
        COMMAND chast-quic verify --port 45506 --terminal --quiet)
    set_tests_properties(quic_terminal PROPERTIES TIMEOUT 120)
    add_test(NAME quic_terminal_denied
        COMMAND chast-quic verify --port 45507 --terminal-denied --quiet)
    set_tests_properties(quic_terminal_denied PROPERTIES TIMEOUT 120)
    # Remote camera: the host enumerates its cameras, answers the list, and
    # when one exists opens it and must stream hardware-encoded access units
    # over CCAM before the viewer stops it. A host without a camera still
    # proves the list round-trip.
    add_test(NAME quic_camera
        COMMAND chast-quic verify --port 45508 --camera --quiet)
    set_tests_properties(quic_camera PROPERTIES TIMEOUT 180)
    add_test(NAME quic_camera_denied
        COMMAND chast-quic verify --port 45509 --camera-denied --quiet)
    set_tests_properties(quic_camera_denied PROPERTIES TIMEOUT 120)
    add_test(NAME quic_stream_options
        COMMAND chast-quic verify --port 45434 --stream-options --quiet)
    set_tests_properties(quic_stream_options PROPERTIES TIMEOUT 120)
    # Idle auto-disconnect: viewer input must keep the session alive across two
    # timeout windows (desktop and terminal input); shell output alone must
    # not reset the timer. A quiet session must be closed by the host, and a
    # host with the timeout off must leave it open. The probe overrides the
    # persisted minutes with a millisecond test value.
    add_test(NAME quic_idle_disconnect
        COMMAND chast-quic verify --port 45522 --idle-disconnect --quiet)
    set_tests_properties(quic_idle_disconnect PROPERTIES TIMEOUT 180)
    # Shared-folder browsing: the viewer lists the host's receive directory and
    # one subfolder, the sandbox refuses escapes, and a viewer-requested
    # download crosses CFIL and is compared with the seeded payload.
    add_test(NAME quic_file_browse
        COMMAND chast-quic verify --port 45510 --file-browse --quiet)
    set_tests_properties(quic_file_browse PROPERTIES TIMEOUT 120)
    # CFIL v2 compression: the host offers a 1 MiB compressible .txt and the
    # viewer must report a v2 block stream whose wire bytes stay far below the
    # file size while the decoded bytes still match the offered digest.
    add_test(NAME quic_file_compression
        COMMAND chast-quic verify --port 45512 --file-download --file-compression --quiet)
    set_tests_properties(quic_file_compression PROPERTIES TIMEOUT 120)
    # Recursive folder download and upload through the real ViewerEngine: the
    # walk must mirror the seeded tree including empty folders, a cancel must
    # end with code 2 and leave no partial file, and a dropped file plus a
    # dropped folder must land byte-for-byte under the browsed folder with the
    # host creating the nested directories. The remote console must open at
    # 80x24, echo a typed marker, accept a resize to 100x30 and close cleanly.
    # Downloads land in the user's Downloads\chast under unique names and are
    # removed again.
    add_test(NAME viewer_folder_download COMMAND chast-viewer-probe)
    set_tests_properties(viewer_folder_download PROPERTIES TIMEOUT 180)
    # Protocol v32: a files-only request must reach streaming with no picture
    # size, no displays and zero frames, and the same shared folder must still
    # list, download and accept an upload. The refuser host (file transfer off)
    # must refuse the Hello with AccessDenial::Declined instead of streaming.
    add_test(NAME viewer_files_only COMMAND chast-viewer-probe --files-only)
    set_tests_properties(viewer_files_only PROPERTIES TIMEOUT 180)
    add_test(NAME viewer_files_only_denied COMMAND chast-viewer-probe --files-only-denied)
    set_tests_properties(viewer_files_only_denied PROPERTIES TIMEOUT 120)
    # Protocol v33 clipboard files: a host copy of a text file, a binary with
    # a non-ASCII name and a folder with a nested tree must reach the viewer
    # as one FileList, download through the ordinary transfer channel, and land
    # byte-for-byte in a staging folder the clipboard sink captures, with the
    # folder tree rebuilt from rel_path. The host clipboard is faked, the sink
    # stands in for CF_HDROP, and the staged copies are removed.
    add_test(NAME viewer_clipboard_files COMMAND chast-viewer-probe --clipboard-files)
    set_tests_properties(viewer_clipboard_files PROPERTIES TIMEOUT 120)
    # Protocol v34 reverse clipboard files: a viewer copy of the same shape is
    # announced as a FileList, uploaded through clipboard-marked offers and
    # staged by the host below one session temp folder with per-entry item
    # directories; the captured CF_HDROP must name the three top items with the
    # tree intact, nothing may echo back to the viewer, and the staging folder
    # must be gone once the session ends.
    add_test(NAME viewer_clipboard_files_up COMMAND chast-viewer-probe --clipboard-files-up)
    set_tests_properties(viewer_clipboard_files_up PROPERTIES TIMEOUT 120)
    # Protocol v69 file conflicts: an existing destination is never renamed
    # silently. The viewer prompts for both directions, Skip keeps the
    # existing file apart from failures, Overwrite replaces it, Cancel drops
    # the queue, and one remembered answer covers uploads and downloads.
    add_test(NAME viewer_file_conflict COMMAND chast-viewer-probe --file-conflict)
    set_tests_properties(viewer_file_conflict PROPERTIES TIMEOUT 180)
    # The FFmpeg backend serves the adapters oneVPL does not cover and is the
    # second attempt on Intel. Pin it to exercise its D3D11VA frame pool, rate
    # programming and replay path instead of Auto's oneVPL choice.
    add_test(NAME quic_loopback_ffmpeg
        COMMAND chast-quic verify --port 45463 --seconds 4 --request-keyframe-ms 1200
                --file-upload --file-download --privacy --encoder ffmpeg --quiet)
    set_tests_properties(quic_loopback_ffmpeg PROPERTIES TIMEOUT 120)
    add_test(NAME quic_stream_options_ffmpeg
        COMMAND chast-quic verify --port 45473 --stream-options --encoder ffmpeg --quiet)
    set_tests_properties(quic_stream_options_ffmpeg PROPERTIES TIMEOUT 120)
    # The merged view builds its canvas from every output, so its handshake,
    # union geometry and no-op merge switch get their own loopback.
    add_test(NAME quic_merged
        COMMAND chast-quic verify --port 45435 --merged --quiet)
    set_tests_properties(quic_merged PROPERTIES TIMEOUT 120)
    # The same loopback over each carrier: the host still terminates QUIC and
    # encodes video, while the viewer's datagrams travel inside TCP/WebSocket.
    add_test(NAME quic_carrier_tcp
        COMMAND chast-quic verify --port 45443 --transport tcp --quiet)
    set_tests_properties(quic_carrier_tcp PROPERTIES TIMEOUT 120)
    add_test(NAME quic_carrier_ws
        COMMAND chast-quic verify --port 45453 --transport ws --quiet)
    set_tests_properties(quic_carrier_ws PROPERTIES TIMEOUT 120)
    # Viewer-side recording: the probe muxes the received access units into an
    # MP4, reopens the files with the container demuxer and checks the sample
    # counts against what the session carried. verify's default 75% resolution
    # probe resizes the stream mid-session, so this run also exercises the
    # rollover into a second segment. Both files are removed afterwards unless
    # --save is also given.
    add_test(NAME quic_recording
        COMMAND chast-quic verify --port 45508 --seconds 4
                --record .chast-quic-record-test.mp4 --quiet)
    set_tests_properties(quic_recording PROPERTIES TIMEOUT 120)
    # The same recording probe with HEVC requested. Where the host cannot
    # encode HEVC (an Intel HD 630 falls back to H.264) the run still records
    # and verifies whatever codec the stream carried: the muxer describes each
    # segment from its own key frame and verify checks that description. On a
    # host with an HEVC encoder the same run exercises the hvcC path.
    add_test(NAME quic_recording_hevc
        COMMAND chast-quic verify --port 45511 --seconds 4
                --record .chast-quic-record-hevc-test.mp4 --codec hevc --quiet)
    set_tests_properties(quic_recording_hevc PROPERTIES TIMEOUT 120)
    # The documented weak-network acceptance as an automated loop: an
    # in-process relay sits between the viewer (--port) and the host
    # (--port + 1) and adds 80 ms + 20 ms jitter, 3% loss and a 1 Mbps
    # bottleneck for eight seconds, then goes clean. The 3 Mbps stream must
    # stay intact while the ABR cuts below the bottleneck and climbs back,
    # and every decision must re-announce the stream.
    add_test(NAME quic_weak_network
        COMMAND chast-quic verify --port 45513 --netem --quiet)
    set_tests_properties(quic_weak_network PROPERTIES TIMEOUT 180)
endif()
