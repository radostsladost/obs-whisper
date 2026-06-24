# ---------------------------------------------------------------------------
# whisper-cli
#
# A standalone command-line transcriber. Enumerates audio input devices with
# Qt Multimedia, captures + resamples them to 16 kHz mono, and runs them
# through the shared whisper-common engine, printing transcripts to stdout.
# ---------------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)

add_executable(whisper-cli
  main.cpp
  device-capture.cpp
  monitor-capture.cpp
  window-accumulator.cpp
  transcription-engine.cpp
)

target_link_libraries(whisper-cli
  PRIVATE
    whisper-common
    Qt6::Core
    Qt6::Multimedia
)

install(TARGETS whisper-cli RUNTIME DESTINATION bin)
