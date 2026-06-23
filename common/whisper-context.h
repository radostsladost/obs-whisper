/*
 * Thin C++ wrapper around a whisper.cpp context.
 *
 * Owns the loaded model and exposes a single blocking transcribe() call.
 * All methods are intended to be used from a single worker thread.
 */

#pragma once

#include <string>
#include <vector>

struct whisper_context;

/* Control whisper.cpp's and ggml's own (very chatty) logging. When `verbose`
 * is false, both are silenced; when true, their default stderr logging is kept.
 * Affects the whole process; call once before loading a model. */
void whisper_set_native_logging(bool verbose);

/* One transcribed segment with start/end times in milliseconds (relative to
 * the start of the audio buffer that was passed to transcribe()). */
struct WhisperSegment {
	int64_t t0_ms;
	int64_t t1_ms;
	std::string text;
};

class WhisperContext {
public:
	WhisperContext() = default;
	~WhisperContext();

	WhisperContext(const WhisperContext &) = delete;
	WhisperContext &operator=(const WhisperContext &) = delete;

	/* Load a ggml/gguf whisper model from disk. Returns false on failure.
	 * Any previously loaded model is released first. */
	bool load(const std::string &model_path, bool use_gpu);

	/* Release the loaded model. */
	void unload();

	bool loaded() const { return ctx != nullptr; }
	const std::string &model_path() const { return loaded_path; }

	/* Transcribe a buffer of mono 16 kHz float samples. Resulting segments
	 * are appended to out. Returns false on failure.
	 *
	 *   language  : ISO code such as "en", or "auto" to auto-detect.
	 *   translate : if true, translate the audio to English.
	 *   n_threads : number of inference threads (>= 1). */
	bool transcribe(const std::vector<float> &samples,
			const std::string &language, bool translate,
			int n_threads, std::vector<WhisperSegment> &out);

private:
	struct whisper_context *ctx = nullptr;
	std::string loaded_path;
};
