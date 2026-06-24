/*
 * transcription-engine.h
 *
 * Owns a single shared whisper model and a worker thread. Audio captured from
 * any number of devices is submitted here as fixed-length windows of 16 kHz
 * mono float samples; the worker transcribes them one at a time (whisper.cpp
 * inference is globally serialized anyway) and prints each resulting segment to
 * stdout, tagged with the originating device.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "whisper-context.h"

struct TranscriptionConfig {
	std::string model_path;
	std::string language = "auto";
	bool translate = false;
	bool use_gpu = false;
	bool timestamps = false;
	bool show_labels = true; /* prefix lines with the source device name */
	int n_threads = 0; /* 0 = auto-detect from hardware concurrency */
};

class TranscriptionEngine {
public:
	TranscriptionEngine() = default;
	~TranscriptionEngine();

	/* Load the model and spin up the worker. Returns false on failure. */
	bool start(const TranscriptionConfig &cfg);

	/* Stop the worker, draining any queued windows first. */
	void stop();

	/* Hand off one window of 16 kHz mono samples captured from `label`,
	 * whose first sample corresponds to `base_ms` on that device's
	 * timeline. Thread-safe; may be called from many capture threads. */
	void submit(std::string label, std::vector<float> samples,
		    int64_t base_ms);

private:
	struct Job {
		std::string label;
		std::vector<float> samples;
		int64_t base_ms;
	};

	void run();
	void emitSegment(const std::string &label, int64_t base_ms,
			 const struct WhisperSegment &seg);

	WhisperContext whisper_;
	TranscriptionConfig cfg_;

	std::thread thread_;
	std::mutex mtx_;
	std::condition_variable cv_;
	std::deque<Job> queue_;
	std::atomic<bool> running_{false};
};
