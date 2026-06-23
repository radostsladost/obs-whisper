/*
 * whisper-log.h
 *
 * A tiny logging shim so the shared whisper code can be reused outside of OBS.
 *
 * By default formatted log lines are written to stderr. A host application
 * (e.g. the OBS plugin) can install its own handler to forward them elsewhere
 * (e.g. blog()). The API is C-compatible so it can be driven from the OBS
 * module's C entry point as well as from C++.
 */

#ifndef OBS_WHISPER_COMMON_WHISPER_LOG_H
#define OBS_WHISPER_COMMON_WHISPER_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

enum wlog_level {
	WLOG_ERROR = 0,
	WLOG_WARNING,
	WLOG_INFO,
	WLOG_DEBUG,
};

/* Receives a fully formatted, single-line message (no trailing newline). */
typedef void (*wlog_handler_t)(int level, const char *msg);

/* Install a log handler. Pass NULL to restore the default (stderr) handler. */
void wlog_set_handler(wlog_handler_t handler);

/* Suppress messages more verbose than `max_level` (e.g. set to WLOG_WARNING to
 * drop INFO/DEBUG). Defaults to WLOG_DEBUG, i.e. everything is shown. */
void wlog_set_max_level(int max_level);

/* printf-style logging routed through the active handler. */
void wlog(int level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 2, 3)))
#endif
	;

#ifdef __cplusplus
}
#endif

#endif /* OBS_WHISPER_COMMON_WHISPER_LOG_H */
