/*
 * window-slicer.h
 *
 * Decides where to cut the next transcription window out of a rolling buffer of
 * mono 16 kHz audio.
 *
 * Whisper is run on fixed-length windows, but cutting at a blind sample offset
 * slices straight through whatever word happens to be spoken at that instant,
 * which makes the same word appear split across two transcripts (e.g. "...am
 * talk" / "baking..."). Instead of cutting at exactly the target length, this
 * looks for a short pause (a run of low-energy samples) near the target
 * boundary and cuts there, so words stay whole.
 */

#pragma once

#include <cstddef>
#include <vector>

/* Returns how many leading samples of `buffer` should be emitted as the next
 * transcription window:
 *
 *   - 0                 -> not yet; accumulate more audio before cutting.
 *   - 1 .. buffer.size()-> cut here (a pause was found near the target, or the
 *                          buffer grew long enough that we force a cut to keep
 *                          latency bounded).
 *
 * `buffer`         : rolling mono 16 kHz audio.
 * `target_samples` : preferred window length in samples (the configured
 *                    "Transcription window" expressed at 16 kHz). */
size_t whisper_next_window(const std::vector<float> &buffer,
			   size_t target_samples);
