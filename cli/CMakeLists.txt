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

# Install location for the CLI. Defaults to a per-user bin directory so a plain
# `cmake --install build` needs no root; override for a system-wide install,
# e.g. -DWHISPER_CLI_DESTINATION=/usr/local/bin (which then requires sudo).
if(NOT DEFINED WHISPER_CLI_DESTINATION)
  if(WIN32)
    set(WHISPER_CLI_DESTINATION "bin")
  else()
    set(WHISPER_CLI_DESTINATION "$ENV{HOME}/.local/bin")
  endif()
endif()

install(TARGETS whisper-cli
  RUNTIME DESTINATION "${WHISPER_CLI_DESTINATION}"
  COMPONENT cli)
