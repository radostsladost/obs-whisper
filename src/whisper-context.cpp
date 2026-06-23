#include "whisper-context.h"

#include <obs-module.h>
#include <whisper.h>

#include <algorithm>
#include <cstring>

WhisperContext::~WhisperContext()
{
	unload();
}

bool WhisperContext::load(const std::string &model_path, bool use_gpu)
{
	unload();

	if (model_path.empty())
		return false;

	struct whisper_context_params cparams =
		whisper_context_default_params();
	cparams.use_gpu = use_gpu;

	ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
	if (!ctx) {
		blog(LOG_ERROR, "[obs-whisper] failed to load model: %s",
		     model_path.c_str());
		return false;
	}

	loaded_path = model_path;
	blog(LOG_INFO, "[obs-whisper] loaded model: %s", model_path.c_str());
	return true;
}

void WhisperContext::unload()
{
	if (ctx) {
		whisper_free(ctx);
		ctx = nullptr;
	}
	loaded_path.clear();
}

bool WhisperContext::transcribe(const std::vector<float> &samples,
				const std::string &language, bool translate,
				int n_threads, std::vector<WhisperSegment> &out)
{
	if (!ctx || samples.empty())
		return false;

	struct whisper_full_params wparams =
		whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

	wparams.print_progress = false;
	wparams.print_special = false;
	wparams.print_realtime = false;
	wparams.print_timestamps = false;
	wparams.translate = translate;
	wparams.single_segment = false;
	wparams.no_context = true;
	wparams.suppress_blank = true;
	wparams.n_threads = std::max(1, n_threads);

	const bool auto_detect = language.empty() || language == "auto";
	/* whisper.cpp uses the literal "auto" plus detect_language for detection. */
	wparams.language = auto_detect ? "auto" : language.c_str();
	wparams.detect_language = auto_detect;

	if (whisper_full(ctx, wparams, samples.data(), (int)samples.size()) !=
	    0) {
		blog(LOG_ERROR, "[obs-whisper] whisper_full() failed");
		return false;
	}

	const int n_segments = whisper_full_n_segments(ctx);
	for (int i = 0; i < n_segments; ++i) {
		const char *text = whisper_full_get_segment_text(ctx, i);
		if (!text)
			continue;

		WhisperSegment seg;
		/* whisper times are in centiseconds (10 ms units). */
		seg.t0_ms = whisper_full_get_segment_t0(ctx, i) * 10;
		seg.t1_ms = whisper_full_get_segment_t1(ctx, i) * 10;
		seg.text = text;

		/* Trim leading/trailing whitespace. */
		size_t start = seg.text.find_first_not_of(" \t\r\n");
		size_t end = seg.text.find_last_not_of(" \t\r\n");
		if (start == std::string::npos) {
			seg.text.clear();
		} else {
			seg.text = seg.text.substr(start, end - start + 1);
		}

		if (!seg.text.empty())
			out.push_back(std::move(seg));
	}

	return true;
}
