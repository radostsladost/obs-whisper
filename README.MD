# obs-whisper

Local, offline speech-to-text with [Whisper](https://github.com/openai/whisper)
(via [whisper.cpp](https://github.com/ggerganov/whisper.cpp)). The repository
builds two front-ends that share one transcription core:

- **`obs-whisper`** — an [OBS Studio](https://obsproject.com/) audio filter that
  transcribes a source's audio and saves the captions to a text file.
- **`whisper-cli`** — a standalone Qt command-line tool that enumerates the
  system's audio input devices, transcribes them, and prints the text to
  stdout.

Both link against the **`whisper-common`** static library, so the model
handling, inference parameters and hallucination/silence filtering live in
exactly one place (`common/`).

## The OBS filter

It does three things:

1. **Acts as an audio filter** — add it to any source that has audio (Mic,
   Desktop Audio, a media source, etc.). The audio is passed through completely
   unchanged, so it never alters your stream/recording.
2. **Applies Whisper to the audio stream** — the audio is resampled to 16 kHz
   mono and transcribed on a background thread so the audio pipeline is never
   blocked.
3. **Saves captions to a `.txt` file** — each transcribed segment is appended
   to the file you choose, optionally with timestamps.

## How it works

```
OBS source audio ──filter_audio──▶ pass-through ──▶ rest of OBS pipeline
                        │
                        ├─ resample to 16 kHz mono
                        ▼
                  ring buffer ──signal──▶ worker thread
                                              │
                                              ├─ whisper.cpp inference
                                              ▼
                                       captions.txt
```

The worker collects audio into fixed-length windows (default 10 s, configurable
1–30 s) and runs Whisper on each window. Transcription happens off the audio
thread; if the machine can't keep up, the oldest buffered audio is dropped so
latency and memory stay bounded.

## Requirements

- A C++17 compiler and CMake **3.16+**.
- A Whisper model in `ggml`/`gguf` format, e.g. `ggml-base.en.bin`. Download one
  from the [whisper.cpp models page](https://huggingface.co/ggerganov/whisper.cpp).
- For the **OBS filter**: OBS Studio **28+** development files (`libobs`).
- For the **CLI**: Qt 6 with the Multimedia module (`qt6-multimedia-dev` on
  Debian/Ubuntu).

`whisper.cpp` itself is fetched and built automatically by CMake. Each
front-end is optional: if `libobs` is missing the plugin is skipped, and if Qt 6
Multimedia is missing the CLI is skipped (a status message says which). You can
also toggle them explicitly with `-DBUILD_OBS_PLUGIN=OFF` / `-DBUILD_CLI=OFF`.

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build
```

If CMake can't find `libobs`, point it at your OBS development files:

```sh
cmake -B build -Dlibobs_DIR=/path/to/obs-studio/build/libobs
```

### GPU acceleration

By default whisper.cpp is built **CPU-only**, and the filter's *Use GPU* toggle
has no effect. Enable a GPU backend at configure time. After changing the
backend, do a clean reconfigure (`rm -rf build`).

**Vulkan (recommended on Linux — works with the driver you already have):**

```sh
# Prerequisites (Ubuntu/Debian): Vulkan loader + a shader compiler
sudo apt install libvulkan-dev glslc        # or: glslang-tools

rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DWHISPER_VULKAN=ON
cmake --build build
cmake --install build
```

**CUDA (NVIDIA, best performance — needs the CUDA toolkit):**

```sh
# Requires nvcc matching your GPU. RTX 50-series (Blackwell, sm_120) needs
# CUDA 12.8 or newer. Install from NVIDIA's CUDA repository, then:
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DWHISPER_CUDA=ON
cmake --build build
cmake --install build
```

To confirm the GPU backend is active, watch the OBS log when the model loads —
whisper.cpp prints lines such as `ggml_vulkan: Found ... GPU` or
`ggml_cuda_init: found 1 CUDA devices`. Make sure the filter's **Use GPU**
option is enabled.

### Install locations

The default install copies the module and its `data/` directory into the
per-user OBS plugin folder:

- **Linux:** `~/.config/obs-studio/plugins/obs-whisper/`
- **Windows:** override `OBS_PLUGIN_DESTINATION` / `OBS_DATA_DESTINATION` to your
  OBS install (e.g. `.../obs-studio/obs-plugins/64bit`).

You can override the destinations:

```sh
cmake --install build --prefix /where/obs/lives \
  -DOBS_PLUGIN_DESTINATION=obs-plugins/64bit \
  -DOBS_DATA_DESTINATION=data/obs-plugins/obs-whisper
```

## Usage

### OBS filter

1. Start OBS. In any audio source, click the gear → **Filters**.
2. Under **Audio/Video Filters**, add **Whisper Captions**.
3. Configure:
   - **Whisper model file** — path to your downloaded `ggml-*.bin` model.
   - **Caption output file (.txt)** — where captions are written (truncated at
     the start of each session).
   - **Language** — pick a language or use **Auto-detect**.
   - **Translate to English** — output English regardless of spoken language.
   - **Transcription window (seconds)** — larger windows = better accuracy but
     higher latency.
   - **Write timestamps** — prefix each line with `[hh:mm:ss.mmm --> ...]`.
   - **Use GPU** — enable GPU acceleration when whisper.cpp was built with it.

Captions begin appearing in the chosen file as you speak.

### Example output

With timestamps enabled:

```
[00:00:00.000 --> 00:00:03.480] Hello and welcome to the stream.
[00:00:03.480 --> 00:00:07.200] Today we're going to build an OBS plugin.
```

### Command-line (`whisper-cli`)

The CLI captures live audio input devices and prints transcripts to stdout.
By default it is quiet — only the transcribed text goes to stdout and nothing
else. Pass `--verbose` to see diagnostics (model loading, per-window audio
levels, whisper/ggml/Qt logs) on stderr.

List the available input devices:

```sh
./build/cli/whisper-cli --list
```

Transcribe **all** input devices (the default) with a model:

```sh
./build/cli/whisper-cli --model /path/to/ggml-base.en.bin
```

Each output line is tagged with the device it came from:

```
[Microphone (USB Audio)] Hello and welcome to the stream.
[Desktop Audio] Today we're going to build an OBS plugin.
```

Useful options (see `--help` for the full list):

| Option              | Meaning                                                  |
| ------------------- | -------------------------------------------------------- |
| `--model <path>`    | Whisper `ggml`/`gguf` model (required unless `--list`).  |
| `--list`            | List input devices and exit.                             |
| `--device <match>`  | Only capture devices whose name contains `<match>` (case-insensitive). Repeatable; default is every device. |
| `--language <code>` | Language code (e.g. `en`, `ru`) or `auto` (default).     |
| `--translate`       | Translate the transcription to English.                  |
| `--timestamps`      | Prefix each line with `[start --> end]`.                 |
| `--window <sec>`    | Transcription window length, 1–30 s (default 10).        |
| `--threads <n>`     | Inference threads (default: auto).                       |
| `--gpu`             | Use the GPU (only if built with CUDA/Vulkan).            |
| `--verbose`, `-v`   | Print diagnostics to stderr (off by default).            |

Example — only the USB mic, in Russian, translated to English, with timestamps:

```sh
./build/cli/whisper-cli --model ggml-base.bin \
  --device USB --language ru --translate --timestamps
```

Press **Ctrl-C** to stop. Because output is line-buffered on stdout, you can
pipe it straight into other tools, e.g. `whisper-cli --model ... | tee log.txt`.

## Notes & limitations

- Captions are produced in rolling windows. To avoid splitting a word across a
  window boundary, each window is cut at a short pause near the configured
  length rather than at an exact time offset, so the boundary lands between
  words. If you speak continuously with no pause for longer than the window,
  a cut is still forced to keep latency bounded.
- Larger models (`small`, `medium`, `large`) are more accurate but need more
  CPU/GPU and memory. Start with `base.en` for English.
- This writes captions to a file; it does not push live captions into the
  RTMP/embedded caption track. That could be a future enhancement.

## Project layout

| Path                         | Purpose                                          |
| ---------------------------- | ------------------------------------------------ |
| `common/whisper-context.*`   | Shared whisper.cpp model wrapper + filtering      |
| `common/whisper-log.*`       | Logging shim (stderr by default; host-overridable) |
| `src/plugin-main.c`          | OBS module entry point / registration            |
| `src/whisper-filter.cpp`     | Audio filter, resampling, worker thread, output  |
| `src/caption-writer.*`       | Shared caption file sink                          |
| `cli/main.cpp`               | CLI entry point, argument parsing, device select |
| `cli/device-capture.*`       | Qt audio capture + resampling per device         |
| `cli/transcription-engine.*` | Shared model + worker thread, prints to stdout   |
| `data/locale/en-US.ini`      | OBS UI strings                                    |
| `CMakeLists.txt`             | Top-level build (fetches whisper.cpp)            |
