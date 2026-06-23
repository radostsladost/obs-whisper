#include "whisper-log.h"

#include <stdarg.h>
#include <stdio.h>

static const char *level_tag(int level)
{
	switch (level) {
	case WLOG_ERROR:
		return "error";
	case WLOG_WARNING:
		return "warning";
	case WLOG_DEBUG:
		return "debug";
	case WLOG_INFO:
	default:
		return "info";
	}
}

static void default_handler(int level, const char *msg)
{
	fprintf(stderr, "[whisper] [%s] %s\n", level_tag(level), msg);
	fflush(stderr);
}

static wlog_handler_t g_handler = default_handler;
static int g_max_level = WLOG_DEBUG;

void wlog_set_handler(wlog_handler_t handler)
{
	g_handler = handler ? handler : default_handler;
}

void wlog_set_max_level(int max_level)
{
	g_max_level = max_level;
}

void wlog(int level, const char *fmt, ...)
{
	char buf[2048];
	va_list args;

	if (level > g_max_level)
		return;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (g_handler)
		g_handler(level, buf);
}
