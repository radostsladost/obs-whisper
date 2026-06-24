/*
 * window-accumulator.h
 *
 * Collects mono 16 kHz samples from a capture source and submits
 * whisper-sized windows to a TranscriptionEngine.
 *
 * Each window is cut at a natural pause near the configured length (via
 * whisper_next_window) rather than at a blind sample offset, so words aren't
 * split across the boundary, and near-silent windows are dropped before they
 * reach whisper. Shared by the microphone path (DeviceCapture) and the sink
 * monitor path (MonitorCapture) so the windowing/silence policy lives in one
 * place.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class TranscriptionEngine;

class WindowAccumulator {
public:
	WindowAccumulator(std::string label, TranscriptionEngine *engine,
			  double window_seconds);

	/* Append `count` mono 16 kHz samples and submit any windows that are
	 * now complete. */
	void feed(const float *samples, size_t count);

	double windowSeconds() const { return window_seconds_; }

private:
	void flush();

	std::string label_;
	TranscriptionEngine *engine_;
	double window_seconds_;
	size_t window_samples_;
	std::vector<float> window_;

	/* Total 16 kHz samples already submitted, for timeline timestamps. */
	int64_t submitted_samples_ = 0;
};
