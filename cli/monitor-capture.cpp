#include "monitor-capture.h"
#include "transcription-engine.h"
#include "whisper-log.h"

#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

#include <cstring>
#include <vector>

namespace {

constexpr int kTargetRate = 16000;

} // namespace

MonitorCapture::MonitorCapture(QString sink_node, QString label,
			       TranscriptionEngine *engine,
			       double window_seconds, bool verbose,
			       QObject *parent)
	: QObject(parent), sink_node_(std::move(sink_node)),
	  label_(std::move(label)), verbose_(verbose),
	  acc_(label_.toStdString(), engine, window_seconds)
{
}

MonitorCapture::~MonitorCapture()
{
	stop();
}

bool MonitorCapture::start()
{
	/* Prefer the native PipeWire recorder; fall back to PulseAudio's parec.
	 * Both stream raw little-endian float32 mono PCM at 16 kHz on stdout, so
	 * no resampling or format conversion is needed on our side. */
	QString program;
	QStringList args;

	const QString pw = QStandardPaths::findExecutable("pw-record");
	const QString pa = QStandardPaths::findExecutable("parec");

	if (!pw.isEmpty()) {
		/* A capture stream only connects to a sink's monitor when
		 * told to; stream.capture.sink=true does that, with the sink
		 * node itself as the target. */
		program = pw;
		args << QStringLiteral("-P")
		     << QStringLiteral("stream.capture.sink=true")
		     << QStringLiteral("--target=%1").arg(sink_node_)
		     << QStringLiteral("--rate=%1").arg(kTargetRate)
		     << QStringLiteral("--channels=1")
		     << QStringLiteral("--format=f32")
		     << QStringLiteral("--raw") << QStringLiteral("-");
	} else if (!pa.isEmpty()) {
		/* PulseAudio exposes a sink's monitor as a source named
		 * "<sink>.monitor". */
		program = pa;
		args << QStringLiteral("--raw")
		     << QStringLiteral("--device=%1.monitor").arg(sink_node_)
		     << QStringLiteral("--rate=%1").arg(kTargetRate)
		     << QStringLiteral("--channels=1")
		     << QStringLiteral("--format=float32le");
	} else {
		wlog(WLOG_ERROR,
		     "[cli] '%s': cannot capture sink audio; neither 'pw-record' nor 'parec' was found in PATH",
		     label_.toUtf8().constData());
		return false;
	}

	proc_ = new QProcess(this);
	proc_->setProcessChannelMode(QProcess::SeparateChannels);
	connect(proc_, &QProcess::readyReadStandardOutput, this,
		&MonitorCapture::onReadyRead);
	connect(proc_, &QProcess::readyReadStandardError, this,
		&MonitorCapture::onReadyReadStderr);

	proc_->start(program, args);
	if (!proc_->waitForStarted(3000)) {
		wlog(WLOG_ERROR, "[cli] '%s': failed to start '%s'",
		     label_.toUtf8().constData(),
		     program.toUtf8().constData());
		delete proc_;
		proc_ = nullptr;
		return false;
	}

	wlog(WLOG_INFO,
	     "[cli] capturing sink monitor of '%s' via %s -> 16 kHz mono (window %.1fs)",
	     sink_node_.toUtf8().constData(),
	     program.toUtf8().constData(), acc_.windowSeconds());
	return true;
}

void MonitorCapture::stop()
{
	if (!proc_)
		return;

	proc_->disconnect(this);
	proc_->terminate();
	if (!proc_->waitForFinished(1000)) {
		proc_->kill();
		proc_->waitForFinished(1000);
	}
	delete proc_;
	proc_ = nullptr;
}

void MonitorCapture::onReadyRead()
{
	if (!proc_)
		return;

	const QByteArray chunk = proc_->readAllStandardOutput();
	if (!chunk.isEmpty())
		partial_.append(chunk);

	const int n_samples = partial_.size() / (int)sizeof(float);
	if (n_samples <= 0)
		return;

	if (!logged_first_audio_) {
		logged_first_audio_ = true;
		wlog(WLOG_INFO, "[cli] '%s': first sink audio received",
		     label_.toUtf8().constData());
	}

	std::vector<float> mono((size_t)n_samples);
	std::memcpy(mono.data(), partial_.constData(),
		    (size_t)n_samples * sizeof(float));

	/* Keep any trailing partial sample for the next read. */
	partial_.remove(0, n_samples * (int)sizeof(float));

	acc_.feed(mono.data(), mono.size());
}

void MonitorCapture::onReadyReadStderr()
{
	if (!proc_)
		return;

	const QByteArray err = proc_->readAllStandardError();
	if (verbose_ && !err.isEmpty()) {
		wlog(WLOG_DEBUG, "[cli] '%s' recorder: %s",
		     label_.toUtf8().constData(),
		     err.trimmed().constData());
	}
}
