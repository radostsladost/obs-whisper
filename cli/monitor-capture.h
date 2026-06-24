/*
 * monitor-capture.h
 *
 * Captures a system audio *sink* ("desktop audio" / whatever is playing through
 * an output device) and feeds it to the transcription engine.
 *
 * Qt Multimedia does not expose sink monitor sources for capture, so on Linux
 * (PipeWire / PulseAudio) we record the sink's monitor through an external
 * recorder -- `pw-record` (with stream.capture.sink), falling back to `parec`
 * on the sink's ".monitor" source -- which streams raw 16 kHz mono float PCM on
 * stdout. This is therefore a Linux-only feature; on platforms without those
 * tools start() fails with a clear message.
 */

#pragma once

#include "window-accumulator.h"

#include <QByteArray>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QProcess;
QT_END_NAMESPACE

class TranscriptionEngine;

class MonitorCapture : public QObject {
	Q_OBJECT

public:
	/* `sink_node` is the PipeWire/PulseAudio sink node name (the output
	 * device id, e.g. "alsa_output.<...>.analog-stereo"); its monitor is
	 * what actually gets captured. `label` is what tags the transcripts. */
	MonitorCapture(QString sink_node, QString label,
		       TranscriptionEngine *engine, double window_seconds,
		       bool verbose, QObject *parent = nullptr);
	~MonitorCapture() override;

	/* Launch the external recorder. Returns false if no recorder is
	 * available or it could not be started. */
	bool start();
	void stop();

	QString label() const { return label_; }

private slots:
	void onReadyRead();
	void onReadyReadStderr();

private:
	QString sink_node_;
	QString label_;
	bool verbose_;

	QProcess *proc_ = nullptr;
	QByteArray partial_;
	WindowAccumulator acc_;

	bool logged_first_audio_ = false;
};
