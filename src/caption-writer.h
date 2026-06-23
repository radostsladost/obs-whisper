/*
 * caption-writer.h
 *
 * A shared, reference-counted caption file sink keyed by file path.
 *
 * Multiple filter instances (e.g. one per audio device) can target the same
 * output file: the first to open it truncates it, and all instances then append
 * through the same handle under a shared lock. This lets captions from several
 * devices be merged into a single file. The file is closed automatically when
 * the last referencing instance releases it.
 */

#pragma once

#include <memory>
#include <string>

class CaptionSink;

/* Acquire a shared sink for `path`. The file is truncated the first time it is
 * opened in a session; later acquisitions of the same path share the handle and
 * append. Returns nullptr if `path` is empty or cannot be opened. */
std::shared_ptr<CaptionSink> caption_sink_acquire(const std::string &path);

/* Append a single line (a newline is added) to the sink, thread-safe. */
void caption_sink_write(const std::shared_ptr<CaptionSink> &sink,
			const std::string &line);
