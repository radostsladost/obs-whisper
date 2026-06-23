# ---------------------------------------------------------------------------
# obs-whisper plugin module
#
# The OBS-specific layer: the audio filter, resampling/worker thread and
# caption file writer. The actual whisper inference lives in whisper-common.
# ---------------------------------------------------------------------------
add_library(${PROJECT_NAME} MODULE
  plugin-main.c
  whisper-filter.cpp
  caption-writer.cpp
)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}")

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    OBS::libobs
    whisper-common
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

install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/
  DESTINATION "${OBS_DATA_DESTINATION}")
