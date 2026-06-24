/*
 * whisper-cli
 *
 * Enumerates the system's audio input devices, captures them with Qt
 * Multimedia, and transcribes them locally with whisper.cpp (via the shared
 * whisper-common library). Each transcribed line is written to stdout, tagged
 * with the device it came from. Diagnostics go to stderr.
 *
 * Usage examples:
 *   whisper-cli --list
 *   whisper-cli --model ggml-base.en.bin
 *   whisper-cli --model ggml-base.en.bin --device "USB" --timestamps
 *   whisper-cli --model ggml-base.en.bin --language ru --translate
 */

#include <QAudioDevice>
#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <memory>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "device-capture.h"
#include "monitor-capture.h"
#include "transcription-engine.h"
#include "whisper-log.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

/* Where our own (gated) log lines go. In quiet mode we redirect the process's
 * stderr to /dev/null to silence chatty third-party libraries, so we keep a
 * handle to the original stderr here and write warnings/errors through it. */
FILE *g_log_stream = nullptr;

const char *level_tag(int level)
{
	switch (level) {
	case WLOG_ERROR:
		return "error";
	case WLOG_WARNING:
		return "warning";
	case WLOG_DEBUG:
		return "debug";
	default:
		return "info";
	}
}

void cli_log_handler(int level, const char *msg)
{
	FILE *out = g_log_stream ? g_log_stream : stderr;
	fprintf(out, "[whisper] [%s] %s\n", level_tag(level), msg);
	fflush(out);
}

void handle_signal(int)
{
	g_stop = 1;
}

void print_usage(const char *argv0)
{
	std::fprintf(stderr,
		     "Usage: %s [options]\n"
		     "\n"
		     "Transcribe audio input devices to stdout using whisper.cpp.\n"
		     "\n"
		     "Options:\n"
		     "  --model <path>      Path to a ggml/gguf whisper model (required\n"
		     "                      unless --list). e.g. ggml-base.en.bin\n"
		     "  --list              List available audio devices and exit\n"
		     "  --device <match>    Only capture input devices whose name contains\n"
		     "                      <match> (case-insensitive), or 'default' for the\n"
		     "                      default input. Repeatable. Default: all input\n"
		     "                      devices (unless --sink is given).\n"
		     "  --sink <match>      Also capture system/desktop audio from output\n"
		     "                      sink(s) whose name contains <match>, or 'default'\n"
		     "                      for the default output. Repeatable. Linux only\n"
		     "                      (records the sink monitor via pw-record / parec).\n"
		     "  --language <code>   Language code (e.g. en, ru) or 'auto'. Default: auto\n"
		     "  --translate         Translate transcription to English\n"
		     "  --timestamps        Prefix each line with [start --> end]\n"
		     "  --no-labels         Don't prefix each line with the source\n"
		     "                      device (mic/sink) name\n"
		     "  --window <seconds>  Transcription window length (default 10)\n"
		     "  --threads <n>       Inference threads (default: auto)\n"
		     "  --gpu               Use GPU (only if built with CUDA/Vulkan)\n"
		     "  --verbose, -v       Print diagnostics (model load, audio levels,\n"
		     "                      whisper/ggml logs) to stderr\n"
		     "  --help              Show this help\n",
		     argv0);
}

void list_devices()
{
	const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
	const QAudioDevice def_in = QMediaDevices::defaultAudioInput();

	if (inputs.isEmpty()) {
		std::printf("No audio input devices found.\n");
	} else {
		std::printf("Audio input devices (capture with --device):\n");
		for (const QAudioDevice &d : inputs) {
			const bool is_default = (d.id() == def_in.id());
			std::printf("  %s%s\n",
				    d.description().toUtf8().constData(),
				    is_default ? "  (default)" : "");
		}
	}

	const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
	const QAudioDevice def_out = QMediaDevices::defaultAudioOutput();

	if (!outputs.isEmpty()) {
		std::printf("\nOutput sinks / system audio (capture with --sink):\n");
		for (const QAudioDevice &d : outputs) {
			const bool is_default = (d.id() == def_out.id());
			std::printf("  %s%s\n",
				    d.description().toUtf8().constData(),
				    is_default ? "  (default)" : "");
		}
	}

	std::fflush(stdout);
}

bool device_matches(const QAudioDevice &d, const QStringList &filters,
		    const QAudioDevice &def_in)
{
	if (filters.isEmpty())
		return true;
	const QString name = d.description();
	const QString id = QString::fromUtf8(d.id());
	for (const QString &f : filters) {
		if (f.compare(QStringLiteral("default"),
			      Qt::CaseInsensitive) == 0 &&
		    d.id() == def_in.id())
			return true;
		if (name.contains(f, Qt::CaseInsensitive) ||
		    id.contains(f, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

/* Sinks are matched against both the (often localized) description and the
 * stable device id, plus the special keyword "default". Unlike inputs, an
 * empty filter list matches nothing: sinks are only captured when explicitly
 * requested with --sink. */
bool sink_matches(const QAudioDevice &d, const QStringList &filters,
		  const QAudioDevice &def_out)
{
	const QString name = d.description();
	const QString id = QString::fromUtf8(d.id());
	for (const QString &f : filters) {
		if (f.compare(QStringLiteral("default"),
			      Qt::CaseInsensitive) == 0 &&
		    d.id() == def_out.id())
			return true;
		if (name.contains(f, Qt::CaseInsensitive) ||
		    id.contains(f, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

} // namespace

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);

	TranscriptionConfig cfg;
	QStringList device_filters;
	QStringList sink_filters;
	double window_seconds = 10.0;
	bool list_only = false;
	bool verbose = false;

	const QStringList args = app.arguments();
	for (int i = 1; i < args.size(); ++i) {
		const QString &a = args[i];
		auto next = [&](const char *opt) -> QString {
			if (i + 1 >= args.size()) {
				std::fprintf(stderr,
					     "Missing value for %s\n", opt);
				std::exit(2);
			}
			return args[++i];
		};

		if (a == "--help" || a == "-h") {
			print_usage(argv[0]);
			return 0;
		} else if (a == "--list") {
			list_only = true;
		} else if (a == "--model") {
			cfg.model_path = next("--model").toStdString();
		} else if (a == "--device") {
			device_filters << next("--device");
		} else if (a == "--sink") {
			sink_filters << next("--sink");
		} else if (a == "--language") {
			cfg.language = next("--language").toStdString();
		} else if (a == "--translate") {
			cfg.translate = true;
		} else if (a == "--timestamps") {
			cfg.timestamps = true;
		} else if (a == "--no-labels") {
			cfg.show_labels = false;
		} else if (a == "--gpu") {
			cfg.use_gpu = true;
		} else if (a == "--verbose" || a == "-v") {
			verbose = true;
		} else if (a == "--window") {
			window_seconds = next("--window").toDouble();
		} else if (a == "--threads") {
			cfg.n_threads = next("--threads").toInt();
		} else {
			std::fprintf(stderr, "Unknown argument: %s\n",
				     a.toUtf8().constData());
			print_usage(argv[0]);
			return 2;
		}
	}

	/* Quiet by default: only transcripts (stdout) are emitted. --verbose
	 * re-enables our diagnostics plus whisper.cpp/ggml/Qt logging on stderr. */
	whisper_set_native_logging(verbose);
	if (verbose) {
		wlog_set_max_level(WLOG_DEBUG);
	} else {
		wlog_set_max_level(WLOG_WARNING);
		QLoggingCategory::setFilterRules(
			QStringLiteral("qt.multimedia.*=false\n"
				       "qt.core.qfuture.*=false"));

		/* Some libraries (libpipewire's spaVisitChoice, ggml-vulkan's
		 * shader-compile notice) write straight to stderr, bypassing all
		 * logging callbacks. Redirect the stderr fd to /dev/null but route
		 * our own warnings/errors to the original stderr so real problems
		 * still surface. */
#ifndef _WIN32
		fflush(stderr);
		const int saved = dup(STDERR_FILENO);
		FILE *devnull = fopen("/dev/null", "w");
		if (saved >= 0 && devnull) {
			g_log_stream = fdopen(saved, "w");
			wlog_set_handler(cli_log_handler);
			dup2(fileno(devnull), STDERR_FILENO);
			fclose(devnull);
		}
#endif
	}

	if (list_only) {
		list_devices();
		return 0;
	}

	if (cfg.model_path.empty()) {
		std::fprintf(stderr,
			     "error: --model is required (or use --list).\n\n");
		print_usage(argv[0]);
		return 2;
	}

	if (window_seconds < 1.0 || window_seconds > 30.0) {
		std::fprintf(stderr,
			     "error: --window must be between 1 and 30 seconds.\n");
		return 2;
	}

	/* Select sources.
	 *
	 * Input devices (mics) are captured by default, but only when the user
	 * hasn't asked for something more specific: giving --sink alone means
	 * "just the sink(s)", while --device narrows the inputs. Sinks are only
	 * captured when explicitly requested with --sink. */
	const bool want_inputs =
		!device_filters.isEmpty() || sink_filters.isEmpty();

	const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
	const QAudioDevice def_in = QMediaDevices::defaultAudioInput();
	std::vector<QAudioDevice> selected_inputs;
	if (want_inputs) {
		for (const QAudioDevice &d : inputs) {
			if (device_matches(d, device_filters, def_in))
				selected_inputs.push_back(d);
		}
	}

	const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
	const QAudioDevice def_out = QMediaDevices::defaultAudioOutput();
	std::vector<QAudioDevice> selected_sinks;
	for (const QAudioDevice &d : outputs) {
		if (sink_matches(d, sink_filters, def_out))
			selected_sinks.push_back(d);
	}

	if (!device_filters.isEmpty() && selected_inputs.empty()) {
		std::fprintf(stderr,
			     "error: no input devices matched the given --device filter(s).\n");
		return 1;
	}
	if (!sink_filters.isEmpty() && selected_sinks.empty()) {
		std::fprintf(stderr,
			     "error: no output sinks matched the given --sink filter(s). Try --list.\n");
		return 1;
	}
	if (selected_inputs.empty() && selected_sinks.empty()) {
		std::fprintf(stderr,
			     "error: no audio sources to capture (no input devices found).\n");
		return 1;
	}

	/* Load the model and start the shared transcription worker. */
	TranscriptionEngine engine;
	if (!engine.start(cfg)) {
		std::fprintf(stderr, "error: failed to start transcription.\n");
		return 1;
	}

	/* Start a capture per selected input device. */
	std::vector<std::unique_ptr<DeviceCapture>> captures;
	for (const QAudioDevice &d : selected_inputs) {
		auto cap = std::make_unique<DeviceCapture>(d, &engine,
							   window_seconds);
		if (cap->start())
			captures.push_back(std::move(cap));
	}

	/* Start a monitor capture per selected output sink (desktop audio). */
	std::vector<std::unique_ptr<MonitorCapture>> monitors;
	for (const QAudioDevice &d : selected_sinks) {
		const QString node = QString::fromUtf8(d.id());
		auto cap = std::make_unique<MonitorCapture>(
			node, d.description(), &engine, window_seconds,
			verbose);
		if (cap->start())
			monitors.push_back(std::move(cap));
	}

	if (captures.empty() && monitors.empty()) {
		std::fprintf(stderr,
			     "error: could not start capture on any source.\n");
		engine.stop();
		return 1;
	}

	wlog(WLOG_INFO,
	     "[cli] transcribing %zu source(s); press Ctrl-C to stop",
	     captures.size() + monitors.size());

	/* Translate Ctrl-C into a clean Qt event-loop shutdown by polling the
	 * signal flag (signal handlers must not call into Qt directly). */
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);

	QTimer poll;
	QObject::connect(&poll, &QTimer::timeout, [&]() {
		if (g_stop)
			app.quit();
	});
	poll.start(100);

	const int rc = app.exec();

	for (auto &cap : captures)
		cap->stop();
	for (auto &mon : monitors)
		mon->stop();
	engine.stop();

	return rc;
}
