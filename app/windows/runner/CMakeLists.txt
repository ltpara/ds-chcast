cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

set(VIEWER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../viewer")

# Session engine and the zero-copy GPU frame sink. Kept in its own target so
# the Flutter runner's /WX and no-exceptions settings do not apply to code that
# uses the standard library's threading primitives.
add_library(chast_viewer_core STATIC
  "${VIEWER_DIR}/gpu_frame_sink.cc"
  "${VIEWER_DIR}/viewer_engine.cc"
  "${VIEWER_DIR}/remote_cursor.cc"
)
target_include_directories(chast_viewer_core PUBLIC "${VIEWER_DIR}")
target_compile_features(chast_viewer_core PUBLIC cxx_std_20)
target_compile_definitions(chast_viewer_core PRIVATE NOMINMAX)
target_compile_options(chast_viewer_core PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(chast_viewer_core PUBLIC chast.codec chast.net chast.audio)
target_link_libraries(chast_viewer_core PRIVATE flutter)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${VIEWER_DIR}/viewer_bridge.cc"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib" "imm32.lib" "comctl32.lib")
target_link_libraries(${BINARY_NAME} PRIVATE chast_viewer_core)
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")
target_include_directories(${BINARY_NAME} PRIVATE "${VIEWER_DIR}")
target_compile_features(${BINARY_NAME} PRIVATE cxx_std_20)

# The QUIC stack is loaded dynamically. Flutter packages the runner directory,
# so the runtime DLL has to sit next to the executable.
if(DEFINED MSQUIC_ROOT AND EXISTS "${MSQUIC_ROOT}/bin/msquic.dll")
  add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MSQUIC_ROOT}/bin/msquic.dll" "$<TARGET_FILE_DIR:${BINARY_NAME}>"
    VERBATIM)
endif()

# Server-assisted sessions spawn the Go tunnel agent from the app directory.
# go build is incremental, so running the target on every build stays cheap and
# picks up agent changes without a CMake reconfigure.
find_program(GO_EXECUTABLE go)
if(GO_EXECUTABLE)
  # The runner's runtime directory next to this CMakeLists, per configuration.
  set(CHAST_TUNNEL_OUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/chast-tunnel.exe")
  add_custom_target(chast_viewer_tunnel_binary ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    COMMAND ${GO_EXECUTABLE} build -trimpath
            -o "${CHAST_TUNNEL_OUT}"
            ./cmd/chast-tunnel
    WORKING_DIRECTORY "${CHAST_ROOT}/server"
    BYPRODUCTS "${CHAST_TUNNEL_OUT}"
    COMMENT "Building chast-tunnel.exe"
    VERBATIM)
  add_dependencies(chast_viewer_tunnel_binary ${BINARY_NAME})
else()
  message(WARNING
    "Go was not found; server-assisted connections will fail until "
    "chast-tunnel.exe is placed next to the viewer.")
endif()

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)
