cmake_minimum_required(VERSION 3.16)

project(obs-whisper VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Emit compile_commands.json so editors / language servers (clangd) can resolve
# include paths for all three components.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Static libraries (whisper.cpp + ggml + our common lib) get linked into a
# shared OBS module, so everything must be position-independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# What to build
#
# Both consumers share the same whisper core in common/. Each is optional and
# only built when its dependencies are available, so you can build just the CLI
# on a machine without OBS, or just the plugin without Qt.
# ---------------------------------------------------------------------------
option(BUILD_OBS_PLUGIN "Build the OBS Studio plugin" ON)
option(BUILD_CLI "Build the standalone Qt CLI transcriber" ON)

# ---------------------------------------------------------------------------
# GPU acceleration (optional) — applies to both the plugin and the CLI.
#
#   -DWHISPER_CUDA=ON    NVIDIA, best performance. Requires the CUDA toolkit.
#   -DWHISPER_VULKAN=ON  Any GPU. Requires Vulkan headers/loader + glslc.
# ---------------------------------------------------------------------------
option(WHISPER_CUDA "Build whisper.cpp with CUDA (NVIDIA) GPU support" OFF)
option(WHISPER_VULKAN "Build whisper.cpp with Vulkan GPU support" OFF)

# ---------------------------------------------------------------------------
# whisper.cpp (fetched and built statically)
# ---------------------------------------------------------------------------
include(FetchContent)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_INSTALL OFF CACHE BOOL "" FORCE)
set(GGML_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(WHISPER_CUDA)
  set(GGML_CUDA ON CACHE BOOL "" FORCE)
  message(STATUS "obs-whisper: building whisper.cpp with CUDA backend")
endif()
if(WHISPER_VULKAN)
  set(GGML_VULKAN ON CACHE BOOL "" FORCE)
  message(STATUS "obs-whisper: building whisper.cpp with Vulkan backend")
endif()

FetchContent_Declare(
  whispercpp
  GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
  GIT_TAG v1.7.4
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(whispercpp)

# whisper.cpp overrides CMAKE_POSITION_INDEPENDENT_CODE internally when built
# as static libraries, so force PIC on the actual targets. Without this the
# static archives cannot be linked into our shared plugin module.
foreach(_whisper_tgt whisper ggml ggml-base ggml-cpu ggml-cuda ggml-vulkan)
  if(TARGET ${_whisper_tgt})
    set_target_properties(${_whisper_tgt} PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Shared whisper core (used by both the plugin and the CLI)
# ---------------------------------------------------------------------------
add_subdirectory(common)

# ---------------------------------------------------------------------------
# OBS Studio plugin (optional)
# ---------------------------------------------------------------------------
if(BUILD_OBS_PLUGIN)
  find_package(libobs QUIET)
  if(libobs_FOUND)
    add_subdirectory(src)
  else()
    message(WARNING
      "BUILD_OBS_PLUGIN is ON but libobs was not found; skipping the OBS plugin. "
      "Point CMake at your OBS dev files with -Dlibobs_DIR=...")
  endif()
endif()

# ---------------------------------------------------------------------------
# Standalone Qt CLI (optional)
# ---------------------------------------------------------------------------
if(BUILD_CLI)
  find_package(Qt6 QUIET COMPONENTS Core Multimedia)
  if(Qt6_FOUND)
    add_subdirectory(cli)
  else()
    message(WARNING
      "BUILD_CLI is ON but Qt6 (Core + Multimedia) was not found; skipping the CLI. "
      "Install the Qt 6 Multimedia development packages to enable it.")
  endif()
endif()
