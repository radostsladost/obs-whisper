#include "device-capture.h"
#include "transcription-engine.h"
#include "whisper-log.h"
#include "window-slicer.h"

#include <QAudioSource>
#include <QIODevice>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

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

/* Convert one interleaved frame's worth of samples to a mono float in [-1, 1]
 * by averaging channels. `frame` points at the first byte of the frame. */
float frame_to_mono(const char *frame, const QAudioFormat &fmt)
{
	const int channels = fmt.channelCount();
	const int bytes_per_sample = fmt.bytesPerSample();
	double sum = 0.0;

	for (int ch = 0; ch < channels; ++ch) {
		const char *p = frame + (size_t)ch * bytes_per_sample;
		double v = 0.0;
		switch (fmt.sampleFormat()) {
		case QAudioFormat::UInt8: {
			const uint8_t s = *reinterpret_cast<const uint8_t *>(p);
			v = ((double)s - 128.0) / 128.0;
			break;
		}
		case QAudioFormat::Int16: {
			int16_t s;
			std::memcpy(&s, p, sizeof(s));
			v = (double)s / 32768.0;
			break;
		}
		case QAudioFormat::Int32: {
			int32_t s;
			std::memcpy(&s, p, sizeof(s));
			v = (double)s / 2147483648.0;
			break;
		}
		case QAudioFormat::Float: {
			float s;
			std::memcpy(&s, p, sizeof(s));
			v = (double)s;
			break;
		}
		default:
			v = 0.0;
			break;
		}
		sum += v;
	}

	return channels > 0 ? (float)(sum / channels) : 0.0f;
}

const char *sample_format_name(QAudioFormat::SampleFormat f)
{
	switch (f) {
	case QAudioFormat::UInt8:
		return "uint8";
	case QAudioFormat::Int16:
		return "int16";
	case QAudioFormat::Int32:
		return "int32";
	case QAudioFormat::Float:
		return "float";
	default:
		return "unknown";
	}
}

} // namespace

DeviceCapture::DeviceCapture(const QAudioDevice &device,
			     TranscriptionEngine *engine, double window_seconds,
			     QObject *parent)
	: QObject(parent), device_(device), label_(device.description()),
	  engine_(engine)
{
	window_seconds_ = window_seconds;
	window_samples_ =
		(size_t)std::max(1.0, window_seconds * (double)kTargetRate);
}

DeviceCapture::~DeviceCapture()
{
	stop();
}

bool DeviceCapture::start()
{
	/* Prefer 16 kHz mono float so no conversion/resampling is needed; fall
	 * back to whatever the device supports and convert ourselves. */
	QAudioFormat fmt;
	fmt.setSampleRate(kTargetRate);
	fmt.setChannelCount(1);
	fmt.setSampleFormat(QAudioFormat::Float);

	if (!device_.isFormatSupported(fmt)) {
		fmt = device_.preferredFormat();
		/* We only know how to read these sample formats. */
		if (fmt.sampleFormat() != QAudioFormat::UInt8 &&
		    fmt.sampleFormat() != QAudioFormat::Int16 &&
		    fmt.sampleFormat() != QAudioFormat::Int32 &&
		    fmt.sampleFormat() != QAudioFormat::Float) {
			QAudioFormat alt = fmt;
			alt.setSampleFormat(QAudioFormat::Int16);
			if (device_.isFormatSupported(alt))
				fmt = alt;
		}
	}

	if (!fmt.isValid() || fmt.sampleRate() <= 0 ||
	    fmt.channelCount() <= 0) {
		wlog(WLOG_ERROR, "[cli] '%s': no usable audio format",
		     label_.toUtf8().constData());
		return false;
	}

	format_ = fmt;
	in_rate_ = (double)fmt.sampleRate();

	source_ = new QAudioSource(device_, fmt, this);
	io_ = source_->start();
	if (!io_) {
		wlog(WLOG_ERROR, "[cli] '%s': failed to start capture",
		     label_.toUtf8().constData());
		delete source_;
		source_ = nullptr;
		return false;
	}

	connect(io_, &QIODevice::readyRead, this, &DeviceCapture::drain);

	/* readyRead is not emitted reliably by every Qt audio backend in pull
	 * mode (notably FFmpeg/PipeWire), so also drain the device on a timer.
	 * Reading when nothing is buffered is harmless. */
	pull_timer_ = new QTimer(this);
	connect(pull_timer_, &QTimer::timeout, this, &DeviceCapture::drain);
	pull_timer_->start(100);

	wlog(WLOG_INFO,
	     "[cli] capturing '%s' @ %d Hz, %d ch, %s -> 16 kHz mono (window %.1fs)",
	     label_.toUtf8().constData(), fmt.sampleRate(), fmt.channelCount(),
	     sample_format_name(fmt.sampleFormat()), window_seconds_);
	return true;
}

void DeviceCapture::stop()
{
	if (pull_timer_) {
		pull_timer_->stop();
		pull_timer_ = nullptr;
	}
	if (io_) {
		io_->disconnect(this);
		io_ = nullptr;
	}
	if (source_) {
		source_->stop();
		source_->deleteLater();
		source_ = nullptr;
	}
}

void DeviceCapture::drain()
{
	if (!io_)
		return;

	const QByteArray chunk = io_->readAll();
	if (!chunk.isEmpty())
		partial_.append(chunk);

	const int frame_bytes = format_.bytesPerFrame();
	if (frame_bytes <= 0)
		return;

	const int n_frames = partial_.size() / frame_bytes;
	if (n_frames <= 0)
		return;

	if (!logged_first_audio_) {
		logged_first_audio_ = true;
		wlog(WLOG_INFO, "[cli] '%s': first audio received (%d frames)",
		     label_.toUtf8().constData(), n_frames);
	}
	received_frames_ += n_frames;

	std::vector<float> mono;
	mono.reserve(n_frames);

	const char *data = partial_.constData();
	for (int i = 0; i < n_frames; ++i)
		mono.push_back(frame_to_mono(data + (size_t)i * frame_bytes,
					     format_));

	/* Keep the trailing partial frame for the next read. */
	partial_.remove(0, n_frames * frame_bytes);

	pushChunk(mono);
}

void DeviceCapture::pushChunk(const std::vector<float> &mono)
{
	const size_t frames = mono.size();
	if (frames == 0)
		return;

	/* Fast path: already at the target rate, no resampling needed. */
	if (std::fabs(in_rate_ - (double)kTargetRate) < 1e-6) {
		window_.insert(window_.end(), mono.begin(), mono.end());
		resample_prev_ = mono.back();
		flushWindows();
		return;
	}

	/* Linear resample to 16 kHz, carrying fractional position and the last
	 * sample across calls. Virtual index -1 refers to the previous chunk's
	 * final sample. */
	const double ratio = in_rate_ / (double)kTargetRate;
	double pos = resample_pos_;
	const float prev = resample_prev_;

	while (true) {
		const int i0 = (int)std::floor(pos);
		if (i0 + 1 > (int)frames - 1)
			break;
		const double frac = pos - i0;
		const float s0 = (i0 < 0) ? prev : mono[i0];
		const float s1 = mono[i0 + 1];
		window_.push_back((float)(s0 + (s1 - s0) * frac));
		pos += ratio;
	}

	resample_pos_ = pos - (double)frames;
	resample_prev_ = mono[frames - 1];

	flushWindows();
}

void DeviceCapture::flushWindows()
{
	/* Cut each window at a pause near the configured length rather than at a
	 * blind sample offset, so a word spoken across the boundary isn't split
	 * between two transcripts. */
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
			     label_.toUtf8().constData(), m.peak, m.rms);
			continue;
		}

		wlog(WLOG_INFO,
		     "[cli] '%s': queuing %.1fs window (peak=%.4f rms=%.4f)",
		     label_.toUtf8().constData(),
		     (double)cut / (double)kTargetRate, m.peak, m.rms);
		engine_->submit(label_.toStdString(), std::move(w), base_ms);
	}
}
