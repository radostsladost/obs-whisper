cmake_minimum_required(VERSION 3.16)

project(obs-whisper VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The plugin is a shared module, so every static library we link into it
# (whisper.cpp + ggml) must be compiled as position-independent code.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# libobs
#
# Provide OBS development files however your platform exposes them, e.g.:
#   -Dlibobs_DIR=/path/to/obs-studio/build/libobs   (build tree), or
#   the system package on Linux (libobs-dev).
# ---------------------------------------------------------------------------
find_package(libobs REQUIRED)

# ---------------------------------------------------------------------------
# whisper.cpp (fetched and built statically)
# ---------------------------------------------------------------------------
include(FetchContent)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  whispercpp
  GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
  GIT_TAG v1.7.4
)
FetchContent_MakeAvailable(whispercpp)

# whisper.cpp overrides CMAKE_POSITION_INDEPENDENT_CODE internally when built
# as static libraries, so force PIC on the actual targets. Without this the
# static archives cannot be linked into our shared plugin module.
foreach(_whisper_tgt whisper ggml ggml-base ggml-cpu)
  if(TARGET ${_whisper_tgt})
    set_target_properties(${_whisper_tgt} PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  endif()
endforeach()

# ---------------------------------------------------------------------------
# The plugin module
# ---------------------------------------------------------------------------
add_library(${PROJECT_NAME} MODULE
  src/plugin-main.c
  src/whisper-filter.cpp
  src/whisper-context.cpp
)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}")

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    OBS::libobs
    whisper
)

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

# ---------------------------------------------------------------------------
# Install
#
# Adjust OBS_PLUGIN_DESTINATION / OBS_DATA_DESTINATION for your platform.
# Linux example below installs into the per-user OBS plugin directory.
# ---------------------------------------------------------------------------
if(NOT DEFINED OBS_PLUGIN_DESTINATION)
  if(WIN32)
    set(OBS_PLUGIN_DESTINATION "obs-plugins/64bit")
    set(OBS_DATA_DESTINATION "data/obs-plugins/${PROJECT_NAME}")
  else()
    set(OBS_PLUGIN_DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/${PROJECT_NAME}/bin/64bit")
    set(OBS_DATA_DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/${PROJECT_NAME}/data")
  endif()
endif()

install(TARGETS ${PROJECT_NAME}
  LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}"
  RUNTIME DESTINATION "${OBS_PLUGIN_DESTINATION}")

install(DIRECTORY data/
  DESTINATION "${OBS_DATA_DESTINATION}")
