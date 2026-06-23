# ---------------------------------------------------------------------------
# whisper-common
#
# The shared transcription core: a thin C++ wrapper around whisper.cpp plus a
# small C-compatible logging shim. Both the OBS plugin and the standalone CLI
# link against this so the model handling, inference parameters and
# hallucination/silence filtering live in exactly one place.
# ---------------------------------------------------------------------------
add_library(whisper-common STATIC
  whisper-log.c
  whisper-context.cpp
  window-slicer.cpp
)

target_include_directories(whisper-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(whisper-common PUBLIC whisper)

set_target_properties(whisper-common PROPERTIES POSITION_INDEPENDENT_CODE ON)
