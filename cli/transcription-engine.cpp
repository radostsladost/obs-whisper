#include "transcription-engine.h"
#include "whisper-log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <utility>

namespace {

/* Bound the backlog so a slow machine can't grow memory without limit. When
 * exceeded the oldest windows are dropped (and a warning logged), matching the
 * "stay bounded, drop old audio" policy of the OBS filter. */
constexpr size_t kMaxQueuedWindows = 16;

std::string format_timestamp(int64_t ms)
{
	if (ms < 0)
		ms = 0;
	const int64_t total_ms = ms;
	const int hours = (int)(total_ms / 3600000);
	const int minutes = (int)((total_ms % 3600000) / 60000);
	const int seconds = (int)((total_ms % 60000) / 1000);
	const int millis = (int)(total_ms % 1000);

	char buf[32];
	std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", hours, minutes,
		      seconds, millis);
	return buf;
}

} // namespace

TranscriptionEngine::~TranscriptionEngine()
{
	stop();
}

bool TranscriptionEngine::start(const TranscriptionConfig &cfg)
{
	cfg_ = cfg;

	if (!whisper_.load(cfg_.model_path, cfg_.use_gpu)) {
		wlog(WLOG_ERROR, "[cli] could not load model: %s",
		     cfg_.model_path.c_str());
		return false;
	}

	running_ = true;
	thread_ = std::thread(&TranscriptionEngine::run, this);
	return true;
}

void TranscriptionEngine::stop()
{
	if (!running_.exchange(false)) {
		/* Never started, or already stopped; still join if needed. */
		if (thread_.joinable())
			thread_.join();
		return;
	}

	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();

	whisper_.unload();
}

void TranscriptionEngine::submit(std::string label, std::vector<float> samples,
				 int64_t base_ms)
{
	{
		std::lock_guard<std::mutex> lk(mtx_);
		queue_.push_back({std::move(label), std::move(samples), base_ms});
		while (queue_.size() > kMaxQueuedWindows) {
			wlog(WLOG_WARNING,
			     "[cli] transcription falling behind; dropping a window from '%s'",
			     queue_.front().label.c_str());
			queue_.pop_front();
		}
	}
	cv_.notify_one();
}

void TranscriptionEngine::emitSegment(const std::string &label, int64_t base_ms,
				      const WhisperSegment &seg)
{
	/* Transcripts go to stdout (clean, line-buffered, flushed) so they can
	 * be piped; all status/diagnostics go to stderr via wlog. */
	const std::string ts =
		cfg_.timestamps
			? "[" + format_timestamp(base_ms + seg.t0_ms) +
				  " --> " + format_timestamp(base_ms + seg.t1_ms) +
				  "] "
			: std::string();
	if (cfg_.show_labels) {
		std::printf("[%s] %s%s\n", label.c_str(), ts.c_str(),
			    seg.text.c_str());
	} else {
		std::printf("%s%s\n", ts.c_str(), seg.text.c_str());
	}
	std::fflush(stdout);
}

void TranscriptionEngine::run()
{
	const int n_threads = cfg_.n_threads > 0
				      ? cfg_.n_threads
				      : (int)std::max(1u,
						      std::min(8u,
							       std::thread::hardware_concurrency()));

	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lk(mtx_);
			cv_.wait(lk, [this] {
				return !queue_.empty() || !running_;
			});
			if (queue_.empty()) {
				if (!running_)
					break;
				continue;
			}
			job = std::move(queue_.front());
			queue_.pop_front();
		}

		std::vector<WhisperSegment> segments;
		if (!whisper_.transcribe(job.samples, cfg_.language,
					 cfg_.translate, n_threads, segments)) {
			wlog(WLOG_WARNING,
			     "[cli] transcription failed for window from '%s'",
			     job.label.c_str());
			continue;
		}

		wlog(WLOG_INFO,
		     "[cli] '%s': whisper produced %zu segment(s)",
		     job.label.c_str(), segments.size());

		for (const auto &seg : segments)
			emitSegment(job.label, job.base_ms, seg);
	}
}
