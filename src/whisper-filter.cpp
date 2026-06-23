/*
 * whisper-filter.cpp
 *
 * The OBS audio filter. It:
 *   1. Acts as an audio filter on any source (passes audio through untouched).
 *   2. Resamples the audio to 16 kHz mono and feeds fixed-length windows to
 *      whisper.cpp on a background worker thread.
 *   3. Writes the resulting captions to a plain text file.
 */

#include "whisper-context.h"

#include <obs-module.h>

#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

/* Whisper always expects 16 kHz mono audio. */
static constexpr uint32_t WHISPER_SR = 16000;

/* Settings keys. */
#define S_MODEL_PATH "model_path"
#define S_OUTPUT_PATH "output_path"
#define S_LANGUAGE "language"
#define S_TRANSLATE "translate"
#define S_USE_GPU "use_gpu"
#define S_TIMESTAMPS "save_timestamps"
#define S_WINDOW_SEC "window_seconds"

struct whisper_filter {
	obs_source_t *context = nullptr;

	/* Source sample rate; audio is downmixed + resampled to 16 kHz mono. */
	uint32_t in_sample_rate = 48000;

	/* Linear-resampler state (touched only on the audio thread). */
	double resample_pos = 0.0;
	float resample_prev = 0.0f;

	/* Worker thread + synchronization. */
	std::thread thread;
	std::mutex mtx;
	std::condition_variable cv;
	bool running = false;

	/* Captured 16 kHz mono audio waiting to be transcribed (under mtx). */
	std::vector<float> buffer;
	uint64_t consumed_samples = 0; /* total 16 kHz samples processed/dropped */
	uint64_t received_frames = 0;  /* diagnostics: total input frames seen */
	bool logged_first_audio = false;
	float raw_peak_max = 0.0f;     /* diagnostics: raw input level */
	uint64_t frames_since_log = 0;

	/* Desired settings (written by update(), read by the worker; under mtx). */
	std::string desired_model_path;
	std::string output_path;
	std::string language = "en";
	bool translate = false;
	bool use_gpu = true;
	bool save_timestamps = true;
	size_t window_samples = WHISPER_SR * 10;
	bool reopen_output = false;

	/* Worker-thread-only state. */
	WhisperContext whisper;
	bool loaded_gpu = true; /* GPU setting the current model was loaded with */
	std::ofstream out_file;
	std::string open_output_path;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static std::string format_timestamp(int64_t ms)
{
	if (ms < 0)
		ms = 0;
	int64_t h = ms / 3600000;
	ms %= 3600000;
	int64_t m = ms / 60000;
	ms %= 60000;
	int64_t s = ms / 1000;
	ms %= 1000;

	char buf[32];
	snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld",
		 (long long)h, (long long)m, (long long)s, (long long)ms);
	return std::string(buf);
}

static void write_segments(whisper_filter *f, uint64_t base_samples,
			   bool save_timestamps,
			   const std::vector<WhisperSegment> &segments)
{
	if (!f->out_file.is_open() || segments.empty())
		return;

	const int64_t base_ms = (int64_t)(base_samples * 1000 / WHISPER_SR);

	for (const auto &seg : segments) {
		if (save_timestamps) {
			f->out_file << "[" << format_timestamp(base_ms + seg.t0_ms)
				    << " --> "
				    << format_timestamp(base_ms + seg.t1_ms)
				    << "] ";
		}
		f->out_file << seg.text << "\n";
		blog(LOG_DEBUG, "[obs-whisper] caption: %s", seg.text.c_str());
	}
	f->out_file.flush();
}

static void open_output_file(whisper_filter *f, const std::string &path)
{
	if (f->out_file.is_open())
		f->out_file.close();
	f->open_output_path = path;

	if (path.empty())
		return;

	/* Truncate on open: each session starts a fresh caption file. */
	f->out_file.open(path, std::ios::out | std::ios::trunc);
	if (!f->out_file.is_open()) {
		blog(LOG_ERROR, "[obs-whisper] cannot open output file: %s",
		     path.c_str());
	} else {
		blog(LOG_INFO, "[obs-whisper] writing captions to: %s",
		     path.c_str());
	}
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                      */
/* ------------------------------------------------------------------ */

static void process_window(whisper_filter *f, std::vector<float> &window,
			   uint64_t base_samples, const std::string &language,
			   bool translate, bool save_timestamps)
{
	const int n_threads =
		std::max(1u, std::min(8u, std::thread::hardware_concurrency()));

	/* Measure signal level so we can tell silence apart from a real
	 * transcription problem. */
	float peak = 0.0f;
	double sum_sq = 0.0;
	for (float s : window) {
		float a = std::fabs(s);
		if (a > peak)
			peak = a;
		sum_sq += (double)s * s;
	}
	const double rms =
		window.empty() ? 0.0 : sqrt(sum_sq / (double)window.size());

	blog(LOG_INFO,
	     "[obs-whisper] transcribing %zu samples (%.1fs) peak=%.4f rms=%.4f",
	     window.size(), window.size() / (double)WHISPER_SR, peak, rms);

	/* Skip near-silent windows: running Whisper on silence wastes CPU and
	 * tends to produce hallucinated text. */
	if (peak < 0.01f) {
		blog(LOG_INFO, "[obs-whisper] window is silent, skipping");
		return;
	}

	std::vector<WhisperSegment> segments;
	if (f->whisper.transcribe(window, language, translate, n_threads,
				  segments)) {
		blog(LOG_INFO, "[obs-whisper] whisper produced %zu segment(s)",
		     segments.size());
		write_segments(f, base_samples, save_timestamps, segments);
	} else {
		blog(LOG_WARNING,
		     "[obs-whisper] transcription returned no result");
	}
}

static void worker_run(whisper_filter *f)
{
	while (true) {
		std::string desired_model;
		std::string out_path;
		std::string language;
		bool translate = false;
		bool use_gpu = true;
		bool save_timestamps = true;
		bool need_reopen = false;
		std::vector<float> window;
		uint64_t base_samples = 0;

		{
			std::unique_lock<std::mutex> lk(f->mtx);
			f->cv.wait(lk, [&] {
				return !f->running || f->reopen_output ||
				       f->desired_model_path !=
					       f->whisper.model_path() ||
				       (f->whisper.loaded() &&
					f->use_gpu != f->loaded_gpu) ||
				       f->buffer.size() >= f->window_samples;
			});

			if (!f->running)
				break;

			desired_model = f->desired_model_path;
			out_path = f->output_path;
			language = f->language;
			translate = f->translate;
			use_gpu = f->use_gpu;
			save_timestamps = f->save_timestamps;

			if (f->reopen_output) {
				need_reopen = true;
				f->reopen_output = false;
			}

			if (f->buffer.size() >= f->window_samples) {
				window.assign(f->buffer.begin(),
					      f->buffer.begin() +
						      f->window_samples);
				f->buffer.erase(f->buffer.begin(),
						f->buffer.begin() +
							f->window_samples);
				base_samples = f->consumed_samples;
				f->consumed_samples += f->window_samples;
			}
		}

		/* Apply output-file changes. */
		if (need_reopen && out_path != f->open_output_path)
			open_output_file(f, out_path);

		/* Apply model changes (path change, or GPU toggle on a loaded
		 * model, both require reloading the Whisper context). */
		const bool path_changed =
			desired_model != f->whisper.model_path();
		const bool gpu_changed =
			!desired_model.empty() && f->whisper.loaded() &&
			use_gpu != f->loaded_gpu;
		if (path_changed || gpu_changed) {
			if (desired_model.empty()) {
				f->whisper.unload();
			} else {
				f->whisper.load(desired_model, use_gpu);
				f->loaded_gpu = use_gpu;
			}
		}

		/* Transcribe. */
		if (!window.empty() && f->whisper.loaded()) {
			process_window(f, window, base_samples, language,
				       translate, save_timestamps);
		}
	}

	/* Flush any trailing audio captured before shutdown. */
	std::vector<float> tail;
	uint64_t base_samples = 0;
	std::string language;
	bool translate;
	bool save_timestamps;
	{
		std::lock_guard<std::mutex> lk(f->mtx);
		tail.swap(f->buffer);
		base_samples = f->consumed_samples;
		language = f->language;
		translate = f->translate;
		save_timestamps = f->save_timestamps;
	}
	if (!tail.empty() && f->whisper.loaded())
		process_window(f, tail, base_samples, language, translate,
			       save_timestamps);

	if (f->out_file.is_open())
		f->out_file.close();
}

/* ------------------------------------------------------------------ */
/* OBS source callbacks                                               */
/* ------------------------------------------------------------------ */

static const char *whisper_filter_name(void *)
{
	return obs_module_text("WhisperFilter");
}

static void whisper_filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<whisper_filter *>(data);

	const char *model = obs_data_get_string(settings, S_MODEL_PATH);
	const char *output = obs_data_get_string(settings, S_OUTPUT_PATH);
	const char *lang = obs_data_get_string(settings, S_LANGUAGE);
	const bool translate = obs_data_get_bool(settings, S_TRANSLATE);
	const bool use_gpu = obs_data_get_bool(settings, S_USE_GPU);
	const bool timestamps = obs_data_get_bool(settings, S_TIMESTAMPS);
	int window_sec = (int)obs_data_get_int(settings, S_WINDOW_SEC);
	if (window_sec < 1)
		window_sec = 1;

	std::lock_guard<std::mutex> lk(f->mtx);
	f->desired_model_path = model ? model : "";
	f->language = (lang && *lang) ? lang : "en";
	f->translate = translate;
	f->use_gpu = use_gpu;
	f->save_timestamps = timestamps;
	f->window_samples = (size_t)window_sec * WHISPER_SR;

	std::string new_output = output ? output : "";
	if (new_output != f->output_path) {
		f->output_path = new_output;
		f->reopen_output = true;
	}
	f->cv.notify_one();
}

static void *whisper_filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new whisper_filter();
	f->context = source;

	/* Determine the source/global audio sample rate. */
	audio_t *audio = obs_get_audio();
	const struct audio_output_info *aoi =
		audio ? audio_output_get_info(audio) : nullptr;
	if (aoi && aoi->samples_per_sec > 0)
		f->in_sample_rate = aoi->samples_per_sec;

	whisper_filter_update(f, settings);

	f->running = true;
	f->thread = std::thread(worker_run, f);

	return f;
}

static void whisper_filter_destroy(void *data)
{
	auto *f = static_cast<whisper_filter *>(data);

	{
		std::lock_guard<std::mutex> lk(f->mtx);
		f->running = false;
	}
	f->cv.notify_one();
	if (f->thread.joinable())
		f->thread.join();

	delete f;
}

static struct obs_audio_data *
whisper_filter_audio(void *data, struct obs_audio_data *audio)
{
	auto *f = static_cast<whisper_filter *>(data);

	if (!audio || audio->frames == 0)
		return audio;

	const uint32_t frames = audio->frames;

	/* Collect the populated channel planes (OBS delivers planar float). */
	const float *planes[MAX_AV_PLANES];
	size_t n_channels = 0;
	for (size_t ch = 0; ch < MAX_AV_PLANES; ++ch) {
		const float *p = reinterpret_cast<const float *>(audio->data[ch]);
		if (p)
			planes[n_channels++] = p;
	}
	if (n_channels == 0)
		return audio;

	/* Downmix to mono. */
	std::vector<float> mono(frames);
	float in_peak = 0.0f;
	for (uint32_t i = 0; i < frames; ++i) {
		float sum = 0.0f;
		for (size_t ch = 0; ch < n_channels; ++ch)
			sum += planes[ch][i];
		float s = sum / (float)n_channels;
		mono[i] = s;
		float a = std::fabs(s);
		if (a > in_peak)
			in_peak = a;
	}

	/* Linear-resample mono from in_sample_rate to 16 kHz, carrying the
	 * fractional position and last sample across buffers for continuity.
	 * Virtual index -1 refers to the previous buffer's last sample. */
	const double ratio = (double)f->in_sample_rate / (double)WHISPER_SR;
	std::vector<float> resampled;
	resampled.reserve((size_t)(frames / ratio) + 2);

	double pos = f->resample_pos;
	const float prev = f->resample_prev;
	while (true) {
		int i0 = (int)std::floor(pos);
		if (i0 + 1 > (int)frames - 1)
			break;
		const double frac = pos - i0;
		const float s0 = (i0 < 0) ? prev : mono[i0];
		const float s1 = mono[i0 + 1];
		resampled.push_back((float)(s0 + (s1 - s0) * frac));
		pos += ratio;
	}
	f->resample_pos = pos - (double)frames;
	f->resample_prev = mono[frames - 1];

	{
		std::lock_guard<std::mutex> lk(f->mtx);

		if (!f->logged_first_audio) {
			f->logged_first_audio = true;
			blog(LOG_INFO,
			     "[obs-whisper] receiving audio: %u in-frames @ %u Hz, %zu channel(s) -> %zu out-samples @ %u Hz",
			     frames, f->in_sample_rate, n_channels,
			     resampled.size(), WHISPER_SR);
		}
		f->received_frames += frames;

		if (in_peak > f->raw_peak_max)
			f->raw_peak_max = in_peak;
		f->frames_since_log += frames;
		if (f->frames_since_log >= f->in_sample_rate * 5) {
			blog(LOG_INFO,
			     "[obs-whisper] raw input peak over last ~5s: %.4f",
			     f->raw_peak_max);
			f->raw_peak_max = 0.0f;
			f->frames_since_log = 0;
		}

		f->buffer.insert(f->buffer.end(), resampled.begin(),
				 resampled.end());

		/* Bound memory/latency if transcription falls behind: drop the
		 * oldest audio while keeping the timeline aligned. */
		const size_t max_buffer = f->window_samples * 6;
		if (f->buffer.size() > max_buffer) {
			const size_t drop = f->buffer.size() - max_buffer;
			f->buffer.erase(f->buffer.begin(),
					f->buffer.begin() + drop);
			f->consumed_samples += drop;
			blog(LOG_WARNING,
			     "[obs-whisper] transcription is falling behind; dropped %zu samples",
			     drop);
		}

		f->cv.notify_one();
	}

	/* Pass the audio through unchanged. */
	return audio;
}

static void whisper_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_LANGUAGE, "en");
	obs_data_set_default_bool(settings, S_TRANSLATE, false);
	obs_data_set_default_bool(settings, S_USE_GPU, true);
	obs_data_set_default_bool(settings, S_TIMESTAMPS, true);
	obs_data_set_default_int(settings, S_WINDOW_SEC, 10);
}

static obs_properties_t *whisper_filter_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, S_MODEL_PATH,
				obs_module_text("ModelPath"), OBS_PATH_FILE,
				"Whisper models (*.bin *.gguf);;All files (*.*)",
				nullptr);

	obs_properties_add_path(props, S_OUTPUT_PATH,
				obs_module_text("OutputPath"), OBS_PATH_FILE_SAVE,
				"Text files (*.txt);;All files (*.*)", nullptr);

	obs_property_t *lang = obs_properties_add_list(
		props, S_LANGUAGE, obs_module_text("Language"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang, obs_module_text("AutoDetect"),
				     "auto");
	obs_property_list_add_string(lang, "English", "en");
	obs_property_list_add_string(lang, "Spanish", "es");
	obs_property_list_add_string(lang, "French", "fr");
	obs_property_list_add_string(lang, "German", "de");
	obs_property_list_add_string(lang, "Italian", "it");
	obs_property_list_add_string(lang, "Portuguese", "pt");
	obs_property_list_add_string(lang, "Dutch", "nl");
	obs_property_list_add_string(lang, "Russian", "ru");
	obs_property_list_add_string(lang, "Chinese", "zh");
	obs_property_list_add_string(lang, "Japanese", "ja");
	obs_property_list_add_string(lang, "Korean", "ko");

	obs_properties_add_bool(props, S_TRANSLATE,
				obs_module_text("TranslateToEnglish"));

	obs_properties_add_int_slider(props, S_WINDOW_SEC,
				      obs_module_text("WindowSeconds"), 1, 30,
				      1);

	obs_properties_add_bool(props, S_TIMESTAMPS,
				obs_module_text("SaveTimestamps"));

	obs_properties_add_bool(props, S_USE_GPU, obs_module_text("UseGPU"));

	return props;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

static struct obs_source_info make_whisper_filter_info()
{
	struct obs_source_info info = {};
	info.id = "whisper_caption_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = whisper_filter_name;
	info.create = whisper_filter_create;
	info.destroy = whisper_filter_destroy;
	info.update = whisper_filter_update;
	info.get_defaults = whisper_filter_defaults;
	info.get_properties = whisper_filter_properties;
	info.filter_audio = whisper_filter_audio;
	return info;
}

extern "C" {
struct obs_source_info whisper_filter_info = make_whisper_filter_info();
}
