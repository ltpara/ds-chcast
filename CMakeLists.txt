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
    src/security/security.cc)
target_include_directories(chast.security PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(chast.security PUBLIC chast.base bcrypt crypt32)

add_library(chast.dxgi STATIC
    src/win/com.cc
    src/capture/display.cc
    src/capture/display_mode.cc
    src/capture/rotation.cc
    src/capture/shader.cc
    src/capture/hdr_pass.cc
    src/capture/rotation_pass.cc
    src/capture/nv12_pass.cc
    src/capture/cursor.cc
    src/capture/dxgi_capturer.cc
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
    src/codec/vpl_encoder.cc
    src/codec/ffmpeg_decoder.cc)

target_include_directories(chast.codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(chast.codec PRIVATE ONEVPL_EXPERIMENTAL)
target_link_libraries(chast.codec PUBLIC chast.dxgi VPL::dispatcher chast.ffmpeg)

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
    src/clipboard/clipboard_win.cc)
target_include_directories(chast.clipboard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(chast.clipboard PUBLIC chast.base chast.security user32)

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
# session lock state behind the desktop tracker.
target_link_libraries(chast.net PUBLIC chast.base chast.security chast.clipboard msquic crypt32 ncrypt ws2_32 ntdll user32 wtsapi32)

# Host product code shared by the tray application and the command-line
# server: settings persistence and the capture/encode/serve loop. It sits on
# top of both chast.codec and chast.net so neither of those has to know about
# the other.
add_library(chast.host STATIC
    src/host/audit_spool.cc
    src/host/bitrate_controller.cc
    src/host/host_security.cc
    src/host/host_settings.cc
    src/host/host_server.cc
    src/host/port_forward_relay.cc
    src/host/terminal_relay.cc
    src/win/privacy.cc
    src/win/sas.cc)

target_include_directories(chast.host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(chast.host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chast.host PUBLIC chast.codec chast.net chast.audio chast.clipboard chast.transfer shell32 ole32)

# Windows integration shared by the tray and the service: firewall/autostart
# helpers, the Go tunnel child process and SCM control. Kept apart from
# chast.host so the command-line tools do not pull in Win32 process management
# just for the settings.
add_library(chast.host.tray STATIC
    host/platform.cc
    host/tunnel.cc
    host/service.cc
    host/service_log.cc)
target_include_directories(chast.host.tray PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host)
target_link_libraries(chast.host.tray PUBLIC chast.host shell32 advapi32 wtsapi32)

# Notification-area host application. It is product code, so it is built even
# when the test tools are switched off.
add_executable(chast-host WIN32 host/main.cc)
target_link_libraries(chast-host PRIVATE chast.host.tray comctl32 comdlg32)
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

    # Raw duplicate/acquire loop: isolates a driver or platform fault from the
    # capturer's own code when debugging.
    add_executable(chast-dup-loop tools/dup_loop.cc tools/crash_handler.cc)
    target_link_libraries(chast-dup-loop PRIVATE d3d11 dxgi dbghelp)

    # Pure-logic checks for the cursor helpers: no duplication, no GPU.
    add_executable(chast-cursor-test tests/cursor_test.cc)
    target_include_directories(chast-cursor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(chast-cursor-test PRIVATE chast.dxgi)

    # The viewer's sprite installer: validation, the scaled-handle cache and
    # release. Builds handles from a screen DC only, so it needs no desktop.
    add_executable(chast-remote-cursor-test tests/remote_cursor_test.cc
                   app/windows/viewer/remote_cursor.cc)
    target_include_directories(chast-remote-cursor-test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app/windows/viewer)
    target_link_libraries(chast-remote-cursor-test PRIVATE chast.dxgi)

    # Pure-logic checks for the wire format and the HID -> scan-code table.
    add_executable(chast-protocol-test tests/protocol_test.cc)
    target_link_libraries(chast-protocol-test PRIVATE chast.net)

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
    target_link_libraries(chast-host-settings-test PRIVATE chast.host)

    # Authentication policy: modes, lockout, temporary password, 2FA,
    # trusted devices, allow list and permission intersection.
    add_executable(chast-host-security-test tests/host_security_test.cc)
    target_link_libraries(chast-host-security-test PRIVATE chast.host)

    # Adaptive bitrate policy: pure logic, driven by synthetic transport
    # samples.
    add_executable(chast-bitrate-test tests/bitrate_test.cc)
    target_link_libraries(chast-bitrate-test PRIVATE chast.host)

    # AAC encode/decode round trip: no device, no network.
    add_executable(chast-audio-codec-test tests/audio_codec_test.cc)
    target_link_libraries(chast-audio-codec-test PRIVATE chast.audio)

    # Echo cancellation and noise suppression against synthetic far/near
    # signals: no device, no network, deterministic.
    add_executable(chast-audio-dsp-test tests/audio_dsp_test.cc)
    target_link_libraries(chast-audio-dsp-test PRIVATE chast.audio)

    # Receive-side file handling: name sanitizing, resumable .part files and
    # digest verification, all inside a temporary directory.
    add_executable(chast-file-transfer-test tests/file_transfer_test.cc)
    target_link_libraries(chast-file-transfer-test PRIVATE chast.transfer)

    # Tunnel event parsing, argument construction and child lifecycle. The
    # lifecycle case relaunches this executable as a fake agent.
    add_executable(chast-host-tunnel-test tests/host_tunnel_test.cc)
    target_link_libraries(chast-host-tunnel-test PRIVATE chast.host.tray)

    # Raw TCP and WebSocket carrier framing against a loopback UDP echo,
    # including the RFC 6455 handshake, bad frames and the connection limit.
    add_executable(chast-carrier-test tests/carrier_test.cc)
    target_link_libraries(chast-carrier-test PRIVATE chast.net)

    enable_testing()
    add_test(NAME cursor COMMAND chast-cursor-test)
    add_test(NAME remote_cursor COMMAND chast-remote-cursor-test)
    add_test(NAME protocol COMMAND chast-protocol-test)
    add_test(NAME desktop COMMAND chast-desktop-test)
    add_test(NAME security COMMAND chast-security-test)
    add_test(NAME host_settings COMMAND chast-host-settings-test)
    add_test(NAME host_security COMMAND chast-host-security-test)
    add_test(NAME host_tunnel COMMAND chast-host-tunnel-test)
    add_test(NAME bitrate COMMAND chast-bitrate-test)
    add_test(NAME audio COMMAND chast-audio-codec-test)
    add_test(NAME audio_dsp COMMAND chast-audio-dsp-test)
    add_test(NAME file_transfer COMMAND chast-file-transfer-test)
    add_test(NAME carrier COMMAND chast-carrier-test)

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
    add_test(NAME quic_stream_options
        COMMAND chast-quic verify --port 45434 --stream-options --quiet)
    set_tests_properties(quic_stream_options PROPERTIES TIMEOUT 120)
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
endif()
