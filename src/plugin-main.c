/*
 * obs-whisper
 *
 * An OBS Studio audio filter that runs OpenAI Whisper (via whisper.cpp)
 * locally on a source's audio stream and writes the resulting captions
 * to a plain text file.
 *
 * This file contains the OBS module boilerplate. The actual filter is
 * defined in whisper-filter.cpp.
 */

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-whisper", "en-US")

/* Defined in whisper-filter.cpp */
extern struct obs_source_info whisper_filter_info;

bool obs_module_load(void)
{
	obs_register_source(&whisper_filter_info);
	blog(LOG_INFO, "[obs-whisper] plugin loaded (version %s)",
	     PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-whisper] plugin unloaded");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS Whisper Captions";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Transcribe a source's audio with Whisper and save captions to a text file.";
}
