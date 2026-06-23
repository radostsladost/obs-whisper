#include "whisper-context.h"

#include <obs-module.h>
#include <whisper.h>

#include <algorithm>
#include <cstring>

/* Returns true if the (already-trimmed) text is a non-speech annotation that
 * Whisper sometimes emits on noise, e.g. "[clicking]", "(music)", "*counting*".
 * Such text is wrapped entirely in brackets/parentheses/asterisks. */
static bool is_non_speech_annotation(const std::string &t)
{
	if (t.size() < 2)
		return false;
	const char first = t.front();
	const char last = t.back();
	const bool wrapped = (first == '[' && last == ']') ||
			     (first == '(' && last == ')') ||
			     (first == '{' && last == '}') ||
			     (first == '*' && last == '*');
	if (!wrapped)
		return false;
	/* Make sure the wrapping encloses the whole string (no closing bracket
	 * in the middle that would indicate real text afterwards). */
	for (size_t i = 1; i + 1 < t.size(); ++i) {
		if (t[i] == last)
			return false;
	}
	return true;
}

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
	wparams.suppress_nst = true; /* suppress non-speech tokens like [clicking] */
	wparams.temperature = 0.0f;
	wparams.temperature_inc = 0.2f;
	wparams.no_speech_thold = 0.6f;
	wparams.entropy_thold = 2.4f;
	wparams.logprob_thold = -1.0f;
	wparams.n_threads = std::max(1, n_threads);

	const bool auto_detect = language.empty() || language == "auto";
	/* whisper.cpp uses the literal "auto" plus detect_language for detection. */
	if (!auto_detect && whisper_lang_id(language.c_str()) < 0) {
		blog(LOG_WARNING,
		     "[obs-whisper] unknown language code '%s', using auto-detect",
		     language.c_str());
		wparams.language = "auto";
		wparams.detect_language = true;
	} else {
		wparams.language = auto_detect ? "auto" : language.c_str();
		wparams.detect_language = auto_detect;
	}

	blog(LOG_INFO,
	     "[obs-whisper] whisper params: language='%s' detect=%d translate=%d",
	     wparams.language ? wparams.language : "(null)",
	     (int)wparams.detect_language, (int)wparams.translate);

	if (whisper_full(ctx, wparams, samples.data(), (int)samples.size()) !=
	    0) {
		blog(LOG_ERROR, "[obs-whisper] whisper_full() failed");
		return false;
	}

	const int lang_id = whisper_full_lang_id(ctx);
	blog(LOG_INFO, "[obs-whisper] detected/used language: %s",
	     lang_id >= 0 ? whisper_lang_str(lang_id) : "(unknown)");

	const int n_segments = whisper_full_n_segments(ctx);
	for (int i = 0; i < n_segments; ++i) {
		/* Drop segments Whisper itself flags as likely non-speech: this
		 * removes most "thank you for watching"-style hallucinations. */
		const float nsp = whisper_full_get_segment_no_speech_prob(ctx, i);
		if (nsp > 0.6f) {
			blog(LOG_INFO,
			     "[obs-whisper] dropping non-speech segment (p=%.2f)",
			     nsp);
			continue;
		}

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

		if (seg.text.empty())
			continue;

		/* Drop bracketed non-speech annotations like "[clicking]". */
		if (is_non_speech_annotation(seg.text)) {
			blog(LOG_INFO,
			     "[obs-whisper] dropping annotation: %s",
			     seg.text.c_str());
			continue;
		}

		out.push_back(std::move(seg));
	}

	return true;
}
