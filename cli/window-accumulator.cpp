#include "window-accumulator.h"
#include "transcription-engine.h"
#include "whisper-log.h"
#include "window-slicer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kTargetRate = 16000;

struct WindowMetrics {
	float peak;
	double rms;
};

WindowMetrics measure_window(const std::vector<float> &w)
{
	if (w.empty())
		return {0.0f, 0.0};
	float peak = 0.0f;
	double sum_sq = 0.0;
	for (float s : w) {
		const float a = std::fabs(s);
		if (a > peak)
			peak = a;
		sum_sq += (double)s * s;
	}
	return {peak, std::sqrt(sum_sq / (double)w.size())};
}

} // namespace

WindowAccumulator::WindowAccumulator(std::string label,
				     TranscriptionEngine *engine,
				     double window_seconds)
	: label_(std::move(label)), engine_(engine),
	  window_seconds_(window_seconds)
{
	window_samples_ =
		(size_t)std::max(1.0, window_seconds * (double)kTargetRate);
}

void WindowAccumulator::feed(const float *samples, size_t count)
{
	if (count == 0)
		return;
	window_.insert(window_.end(), samples, samples + count);
	flush();
}

void WindowAccumulator::flush()
{
	/* Cut each window at a pause near the configured length rather than at
	 * a blind sample offset, so a word spoken across the boundary isn't
	 * split between two transcripts. */
	for (;;) {
		const size_t cut = whisper_next_window(window_, window_samples_);
		if (cut == 0)
			break;

		std::vector<float> w(window_.begin(), window_.begin() + cut);
		window_.erase(window_.begin(), window_.begin() + cut);

		const int64_t base_ms =
			submitted_samples_ * 1000 / kTargetRate;
		submitted_samples_ += (int64_t)cut;

		/* Skip near-silent windows: running whisper on silence wastes
		 * time and tends to hallucinate text. */
		const WindowMetrics m = measure_window(w);
		if (m.peak < 0.02f || m.rms < 0.005) {
			wlog(WLOG_INFO,
			     "[cli] '%s': window silent (peak=%.4f rms=%.4f), skipping",
			     label_.c_str(), m.peak, m.rms);
			continue;
		}

		wlog(WLOG_INFO,
		     "[cli] '%s': queuing %.1fs window (peak=%.4f rms=%.4f)",
		     label_.c_str(), (double)cut / (double)kTargetRate, m.peak,
		     m.rms);
		engine_->submit(label_, std::move(w), base_ms);
	}
}
