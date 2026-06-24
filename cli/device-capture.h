/*
 * device-capture.h
 *
 * Captures audio from a single Qt audio input device, downmixes it to mono,
 * resamples it to 16 kHz and feeds fixed-length windows to a TranscriptionEngine.
 *
 * Qt does not resample for us, so we accept whatever sample rate / format the
 * device offers (preferring 16 kHz mono float) and convert ourselves. A
 * stateful linear resampler carries the fractional read position and last
 * sample across buffers so there are no clicks at buffer boundaries.
 */

#pragma once

#include "window-accumulator.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <vector>

QT_BEGIN_NAMESPACE
class QAudioSource;
class QIODevice;
class QTimer;
QT_END_NAMESPACE

class TranscriptionEngine;

class DeviceCapture : public QObject {
	Q_OBJECT

public:
	DeviceCapture(const QAudioDevice &device, TranscriptionEngine *engine,
		      double window_seconds, QObject *parent = nullptr);
	~DeviceCapture() override;

	/* Open the device and begin streaming. Returns false if the device
	 * cannot be opened in any usable format. */
	bool start();
	void stop();

	QString label() const { return label_; }

private slots:
	void drain();

private:
	void pushChunk(const std::vector<float> &mono);

	QAudioDevice device_;
	QString label_;
	TranscriptionEngine *engine_;

	QAudioFormat format_;
	QAudioSource *source_ = nullptr;
	QIODevice *io_ = nullptr;
	QTimer *pull_timer_ = nullptr;

	/* Windowing / slicing / submission to the engine. */
	WindowAccumulator acc_;

	/* Diagnostics. */
	bool logged_first_audio_ = false;
	int64_t received_frames_ = 0;

	/* Linear-resampler state (input rate -> 16 kHz). */
	double in_rate_ = 16000.0;
	double resample_pos_ = 0.0;
	float resample_prev_ = 0.0f;

	/* Bytes left over from a read that didn't end on a frame boundary. */
	QByteArray partial_;
};
