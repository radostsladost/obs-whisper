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

#include "whisper-log.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-whisper", "en-US")

/* Defined in whisper-filter.cpp */
extern struct obs_source_info whisper_filter_info;

/* Forward log lines from the shared whisper code into the OBS log. */
static void obs_whisper_log_handler(int level, const char *msg)
{
	int obs_level;
	switch (level) {
	case WLOG_ERROR:
		obs_level = LOG_ERROR;
		break;
	case WLOG_WARNING:
		obs_level = LOG_WARNING;
		break;
	case WLOG_DEBUG:
		obs_level = LOG_DEBUG;
		break;
	case WLOG_INFO:
	default:
		obs_level = LOG_INFO;
		break;
	}
	blog(obs_level, "%s", msg);
}

bool obs_module_load(void)
{
	wlog_set_handler(obs_whisper_log_handler);
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
