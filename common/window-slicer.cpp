#include "window-slicer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr size_t kSampleRate = 16000;

/* How far either side of the target boundary we look for a pause. The window
 * may therefore end up to this much shorter or longer than the configured
 * length, which is what lets the cut land in a gap between words. */
constexpr size_t kSearchRadius = kSampleRate * 2; /* 2 s */

/* A pause must be at least this long to count as a word/phrase boundary; this
 * skips the brief dips inside a single word (plosives, etc.). */
constexpr size_t kMinGapSamples = kSampleRate * 150 / 1000; /* 150 ms */

} // namespace

size_t whisper_next_window(const std::vector<float> &buffer, size_t target)
{
	const size_t n = buffer.size();

	if (target == 0)
		return n; /* degenerate config: emit whatever we have */
	if (n < target)
		return 0; /* not enough audio to consider a cut yet */

	/* Region of the buffer to search for a pause: a band centred on the
	 * target boundary. Keep a sane lower bound so we never emit an
	 * absurdly short window. */
	const size_t lo = (target > kSearchRadius)
				  ? target - kSearchRadius
				  : std::max<size_t>(1, target / 2);
	const size_t hi = std::min(n, target + kSearchRadius);

	/* Adaptive quiet threshold: a between-word pause sits well below the
	 * speech peak, so key off that peak rather than a fixed absolute level
	 * (which would misbehave with a noisy mic or a very quiet speaker). */
	float peak = 0.0f;
	for (size_t i = lo; i < hi; ++i)
		peak = std::max(peak, std::fabs(buffer[i]));
	const float quiet = std::max(0.01f, 0.2f * peak);

	/* Find the pause whose centre is closest to the target boundary, so the
	 * emitted window stays as near the configured length as possible while
	 * still cutting in silence. */
	size_t best_cut = 0;
	size_t best_dist = SIZE_MAX;
	size_t run_start = lo;
	bool in_run = false;

	for (size_t i = lo; i <= hi; ++i) {
		const bool quiet_here =
			(i < hi) && (std::fabs(buffer[i]) < quiet);

		if (quiet_here && !in_run) {
			in_run = true;
			run_start = i;
		} else if (!quiet_here && in_run) {
			in_run = false;
			const size_t run_len = i - run_start;
			if (run_len >= kMinGapSamples) {
				const size_t centre = run_start + run_len / 2;
				const size_t dist = centre > target
							    ? centre - target
							    : target - centre;
				if (dist < best_dist) {
					best_dist = dist;
					best_cut = centre;
				}
			}
		}
	}

	if (best_cut != 0)
		return best_cut;

	/* No pause anywhere near the boundary. If the buffer has already grown
	 * past the search band the speaker simply isn't pausing; force a cut at
	 * the target to keep latency and memory bounded. Otherwise wait a little
	 * longer in the hope a pause appears. */
	if (n >= target + kSearchRadius)
		return target;

	return 0;
}
