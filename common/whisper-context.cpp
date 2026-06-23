#include "whisper-context.h"
#include "whisper-log.h"

#include <ggml.h>
#include <whisper.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

/*
 * The ggml GPU backends (Vulkan/CUDA) share global device state whose
 * initialization is not thread-safe. With one filter instance per audio device,
 * multiple worker threads can call into whisper.cpp concurrently. Serialize all
 * model load/unload/inference across instances to avoid races (notably a crash
 * when two instances initialize the GPU backend at the same time).
 */
static std::mutex g_whisper_global_mutex;

static void whisper_silent_log(enum ggml_log_level, const char *, void *) {}

void whisper_set_native_logging(bool verbose)
{
	if (verbose)
		return; /* keep whisper.cpp / ggml default stderr logging */

	whisper_log_set(whisper_silent_log, nullptr);
	ggml_log_set(whisper_silent_log, nullptr);
}

/* Lowercase an UTF-8 string for ASCII and the Russian Cyrillic range so that
 * hallucination matching is case-insensitive for the languages we care about. */
static std::string utf8_lower(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = (unsigned char)s[i];
		if (c < 0x80) {
			out += (char)std::tolower(c);
			++i;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
			unsigned cp = ((c & 0x1F) << 6) |
				      ((unsigned char)s[i + 1] & 0x3F);
			if (cp >= 0x0410 && cp <= 0x042F) /* А-Я -> а-я */
				cp += 0x20;
			else if (cp == 0x0401) /* Ё -> ё */
				cp = 0x0451;
			out += (char)(0xC0 | (cp >> 6));
			out += (char)(0x80 | (cp & 0x3F));
			i += 2;
		} else {
			out += s[i];
			++i;
		}
	}
	return out;
}

/* Returns true if `text` is one of Whisper's well-known hallucinations that it
 * emits on silence or background noise ("Thanks for watching!", "Спасибо за
 * внимание!", "Пока!", ...). Compared case-insensitively after stripping
 * surrounding punctuation and whitespace. */
static bool is_hallucination_phrase(const std::string &text)
{
	/* Strip leading/trailing whitespace and punctuation the model tends to
	 * append to these stock phrases. */
	static const char *kStrip = " \t\r\n.!?,;:\"'\u2026\u00ab\u00bb-";
	size_t start = text.find_first_not_of(kStrip);
	if (start == std::string::npos)
		return true; /* punctuation/whitespace only */
	size_t end = text.find_last_not_of(kStrip);
	const std::string core =
		utf8_lower(text.substr(start, end - start + 1));

	static const std::set<std::string> kPhrases = {
		/* Russian */
		"спасибо за внимание",
		"спасибо за просмотр",
		"спасибо",
		"пока",
		"пока пока",
		"продолжение следует",
		"до новых встреч",
		"подписывайтесь на канал",
		"субтитры сделал dimatorzok",
		"субтитры создавал dimatorzok",
		/* English */
		"thank you",
		"thank you.",
		"thanks for watching",
		"thank you for watching",
		"please subscribe",
		"subscribe to my channel",
		"bye",
		"bye bye",
		"you",
		/* Other common ones */
		"ご視聴ありがとうございました",
		"\u0936\u0941\u0915\u094d\u0930\u093f\u092f\u093e",
	};

	return kPhrases.count(core) > 0;
}

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

	std::lock_guard<std::mutex> lk(g_whisper_global_mutex);
	ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
	if (!ctx) {
		wlog(WLOG_ERROR, "[whisper] failed to load model: %s",
		     model_path.c_str());
		return false;
	}

	loaded_path = model_path;
	wlog(WLOG_INFO, "[whisper] loaded model: %s", model_path.c_str());
	return true;
}

void WhisperContext::unload()
{
	if (ctx) {
		std::lock_guard<std::mutex> lk(g_whisper_global_mutex);
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
	/* Set language to "auto" so whisper.cpp auto-detects it, or to a specific
	 * ISO code. detect_language MUST stay false: when true, whisper.cpp only
	 * runs language detection and returns *without transcribing* (which would
	 * yield zero segments). "auto" already triggers detection followed by
	 * normal transcription. */
	if (!auto_detect && whisper_lang_id(language.c_str()) < 0) {
		wlog(WLOG_WARNING,
		     "[whisper] unknown language code '%s', using auto-detect",
		     language.c_str());
		wparams.language = "auto";
	} else {
		wparams.language = auto_detect ? "auto" : language.c_str();
	}
	wparams.detect_language = false;

	wlog(WLOG_INFO,
	     "[whisper] whisper params: language='%s' detect=%d translate=%d",
	     wparams.language ? wparams.language : "(null)",
	     (int)wparams.detect_language, (int)wparams.translate);

	/* Serialize inference: the shared GPU backend is not safe for concurrent
	 * use across instances. */
	std::lock_guard<std::mutex> lk(g_whisper_global_mutex);

	if (whisper_full(ctx, wparams, samples.data(), (int)samples.size()) !=
	    0) {
		wlog(WLOG_ERROR, "[whisper] whisper_full() failed");
		return false;
	}

	const int lang_id = whisper_full_lang_id(ctx);
	wlog(WLOG_INFO, "[whisper] detected/used language: %s",
	     lang_id >= 0 ? whisper_lang_str(lang_id) : "(unknown)");

	const int n_segments = whisper_full_n_segments(ctx);
	for (int i = 0; i < n_segments; ++i) {
		/* Drop segments Whisper itself flags as likely non-speech: this
		 * removes most "thank you for watching"-style hallucinations. */
		const float nsp = whisper_full_get_segment_no_speech_prob(ctx, i);
		if (nsp > 0.6f) {
			wlog(WLOG_INFO,
			     "[whisper] dropping non-speech segment (p=%.2f)",
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
			wlog(WLOG_INFO, "[whisper] dropping annotation: %s",
			     seg.text.c_str());
			continue;
		}

		/* Drop well-known silence hallucinations ("Спасибо за
		 * внимание!", "Пока!", "Thanks for watching!", ...). */
		if (is_hallucination_phrase(seg.text)) {
			wlog(WLOG_INFO, "[whisper] dropping hallucination: %s",
			     seg.text.c_str());
			continue;
		}

		out.push_back(std::move(seg));
	}

	return true;
}
