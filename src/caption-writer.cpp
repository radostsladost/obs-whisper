#include "caption-writer.h"

#include <obs-module.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

/*
 * One open caption file. Writes are serialized so that several worker threads
 * (one per filter instance) can append concurrently without interleaving
 * partial lines.
 */
class CaptionSink {
public:
	CaptionSink(const std::string &path, bool truncate) : path_(path)
	{
		std::ios::openmode mode = std::ios::out;
		mode |= truncate ? std::ios::trunc : std::ios::app;
		stream_.open(path, mode);
	}

	bool ok() const { return stream_.is_open(); }
	const std::string &path() const { return path_; }

	void write(const std::string &line)
	{
		std::lock_guard<std::mutex> lk(mutex_);
		if (stream_.is_open()) {
			/* Build the whole record first and emit it with a single
			 * insertion so a line is never split mid-way. */
			std::string record = line;
			record += '\n';
			stream_.write(record.data(),
				      (std::streamsize)record.size());
			stream_.flush();
		}
	}

private:
	std::string path_;
	std::ofstream stream_;
	std::mutex mutex_;
};

/* Registry of currently-open sinks, so instances targeting the same path share
 * one handle. Weak pointers let a sink close itself once no instance holds it.
 * Keyed by canonical absolute path so different spellings of the same file
 * (relative paths, symlinks, "./", ...) resolve to a single shared sink. */
static std::mutex g_registry_mutex;
static std::map<std::string, std::weak_ptr<CaptionSink>> g_registry;
/* Canonical paths already truncated during this process. The file is cleared
 * the first time it is opened in a session and appended to afterwards, so a
 * re-acquired sink never wipes captions another instance already wrote. */
static std::set<std::string> g_truncated_paths;

/* Resolve `path` to a stable key. Falls back to the original string if the
 * filesystem cannot canonicalize it (e.g. parent directory missing). */
static std::string canonical_key(const std::string &path)
{
	std::error_code ec;
	std::filesystem::path p = std::filesystem::absolute(path, ec);
	if (ec)
		return path;
	std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
	if (ec)
		return p.string();
	return c.string();
}

std::shared_ptr<CaptionSink> caption_sink_acquire(const std::string &path)
{
	if (path.empty())
		return nullptr;

	const std::string key = canonical_key(path);

	std::lock_guard<std::mutex> lk(g_registry_mutex);

	auto it = g_registry.find(key);
	if (it != g_registry.end()) {
		if (auto existing = it->second.lock())
			return existing; /* share the already-open handle */
	}

	/* Truncate only the first time we open this file this session. */
	const bool truncate = g_truncated_paths.insert(key).second;

	auto sink = std::make_shared<CaptionSink>(path, truncate);
	if (!sink->ok()) {
		blog(LOG_ERROR, "[obs-whisper] cannot open caption file: %s",
		     path.c_str());
		return nullptr;
	}

	g_registry[key] = sink;
	blog(LOG_INFO, "[obs-whisper] caption file ready: %s", path.c_str());
	return sink;
}

void caption_sink_write(const std::shared_ptr<CaptionSink> &sink,
			const std::string &line)
{
	if (sink)
		sink->write(line);
}
