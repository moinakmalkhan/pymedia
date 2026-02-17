# python-media

In-memory video processing library for Python, powered by FFmpeg. No temporary files, no subprocesses — everything runs in-process via ctypes.

```bash
pip install python-media
```

```python
from pymedia import extract_audio, get_video_info, trim_video
```

## Features

- **Extract audio** — pull audio from video as mp3, wav, aac, or ogg
- **Video info** — get duration, resolution, codecs, fps, bitrate, etc.
- **Convert format** — remux to mp4, mkv, webm, avi, mov (fast, no re-encoding)
- **Trim video** — cut a time segment
- **Mute video** — strip all audio tracks
- **Compress video** — re-encode with H.264 at a target quality (CRF)
- **Resize video** — change resolution
- **Extract frame** — grab a single frame as JPEG or PNG
- **Video to GIF** — convert video (or a segment) to animated GIF

## Installation

### Quick install (recommended)

The install script auto-detects your OS, installs FFmpeg dev libraries, and installs python-media — all in one command:

```bash
git clone https://github.com/moinakmalkhan/pymedia.git
cd pymedia
./install.sh
```

### Manual install

If you prefer to install step by step, first install the system dependencies for your platform, then install python-media.

#### Step 1: Install system dependencies

<details>
<summary><b>Ubuntu / Debian / Linux Mint / Pop!_OS</b></summary>

```bash
sudo apt update
sudo apt install gcc pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev \
    libswresample-dev libswscale-dev
```
</details>

<details>
<summary><b>Fedora</b></summary>

```bash
sudo dnf install gcc pkg-config \
    ffmpeg-free-devel libavcodec-free-devel libavformat-free-devel \
    libavutil-free-devel libswresample-free-devel libswscale-free-devel
```

Or with RPM Fusion enabled (for full codec support):

```bash
sudo dnf install gcc pkg-config ffmpeg-devel
```
</details>

<details>
<summary><b>CentOS / RHEL / Rocky / AlmaLinux</b></summary>

```bash
sudo dnf install gcc pkg-config ffmpeg-devel
```
</details>

<details>
<summary><b>Arch Linux / Manjaro / EndeavourOS</b></summary>

```bash
sudo pacman -S gcc pkg-config ffmpeg
```
</details>

<details>
<summary><b>openSUSE</b></summary>

```bash
sudo zypper install gcc pkg-config ffmpeg-devel
```
</details>

<details>
<summary><b>macOS (Homebrew)</b></summary>

```bash
brew install gcc pkg-config ffmpeg
```
</details>

<details>
<summary><b>Windows (via WSL)</b></summary>

python-media does not support Windows natively. Use [WSL (Windows Subsystem for Linux)](https://learn.microsoft.com/en-us/windows/wsl/install):

```powershell
wsl --install
```

Then inside WSL, follow the Ubuntu/Debian instructions above.
</details>

#### Step 2: Install python-media

The C library is compiled automatically during `pip install` — no need to run `make` manually:

```bash
git clone https://github.com/moinakmalkhan/pymedia.git
cd pymedia
pip install .
```

For development (editable install):

```bash
git clone https://github.com/moinakmalkhan/pymedia.git
cd pymedia
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
```

### Verify installation

```bash
python -c "from pymedia import get_video_info; print('python-media installed successfully')"
```

## Usage

```python
from pymedia import (
    extract_audio, get_video_info, convert_format,
    trim_video, mute_video, compress_video, resize_video,
    extract_frame, video_to_gif,
)

with open("video.mp4", "rb") as f:
    data = f.read()

# Get video metadata
info = get_video_info(data)
print(info["duration"], info["width"], info["height"])

# Extract audio as mp3
mp3 = extract_audio(data, format="mp3")

# Convert to webm
webm = convert_format(data, format="webm")

# Trim first 10 seconds
clip = trim_video(data, start=0, end=10)

# Remove audio
silent = mute_video(data)

# Compress (lower CRF = better quality)
small = compress_video(data, crf=28, preset="fast")

# Resize to 720p width
resized = resize_video(data, width=1280)

# Extract a frame at 5 seconds as JPEG
frame = extract_frame(data, timestamp=5.0, format="jpeg")

# Convert to GIF (320px wide, 10fps, first 3 seconds)
gif = video_to_gif(data, width=320, fps=10, start=0, duration=3)
```

## Supported formats

| Function | Formats |
|---|---|
| `extract_audio` | mp3, wav, aac, ogg |
| `convert_format` | mp4, mkv, webm, avi, mov, flv, ts |
| `extract_frame` | jpeg, png |
| `compress_video` / `resize_video` | H.264 mp4 output |
| `video_to_gif` | GIF |

## Platform support

| Platform | Status |
|---|---|
| Linux (x86_64) | Fully supported |
| Linux (ARM64) | Supported (build from source) |
| macOS (Homebrew) | Supported (build from source) |
| Windows (WSL) | Supported via WSL |
| Windows (native) | Not supported |

## Contributing

Contributions are welcome! python-media is open source and we appreciate help from the community.

### Setting up the development environment

1. **Fork the repo** on GitHub and clone your fork:

```bash
git clone https://github.com/<your-username>/pymedia.git
cd pymedia
```

2. **Install system dependencies** (see [Installation](#installation) for your platform).

3. **Create a virtual environment and install in dev mode:**

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
pip install pytest
```

4. **Run the tests to make sure everything works:**

```bash
pytest tests/ -v
```

### Making changes

1. **Create a branch** for your change:

```bash
git checkout -b my-feature
```

2. **Make your changes.** If you're adding a new feature:
   - Add the C function in `src/pymedia/_lib/pymedia.c`
   - Add ctypes bindings in `src/pymedia/_core.py`
   - Add the Python wrapper in the appropriate module (`audio.py`, `video.py`, `frames.py`, or `info.py`)
   - Export it from `src/pymedia/__init__.py`
   - Add tests in `tests/`

3. **Rebuild after any C changes:**

```bash
pip install -e .
```

4. **Run the tests:**

```bash
pytest tests/ -v
```

5. **Commit and push:**

```bash
git add <files>
git commit -m "Short description of the change"
git push origin my-feature
```

6. **Open a Pull Request** on GitHub.

### Project structure

```
src/pymedia/
├── __init__.py    # Public API exports
├── _core.py       # ctypes bindings (loads libpymedia.so)
├── audio.py       # extract_audio
├── video.py       # convert_format, compress, resize, trim, mute, to_gif
├── info.py        # get_video_info
├── frames.py      # extract_frame
└── _lib/
    ├── pymedia.c      # All C code (FFmpeg operations)
    └── libpymedia.so  # Built automatically by `pip install` (not committed to git)
```

### Guidelines

- Keep Python wrappers thin — heavy lifting goes in the C code
- Every new feature needs at least one test
- Tests must not require external files — generate test data in `tests/conftest.py`
- Run `pytest tests/ -v` before submitting a PR and make sure all tests pass

### Ideas for contributions

- Add new video operations (watermark, rotate, change speed, reverse, merge)
- Improve GIF quality (palette generation)
- Add Windows native support
- Migrate deprecated FFmpeg API calls to the new channel layout API
- Expand CI/CD pipeline (test matrix, pre-built wheels)
- Improve error messages from the C layer

### Reporting bugs

Open an issue at https://github.com/moinakmalkhan/pymedia/issues with:
- What you did
- What you expected
- What happened instead
- Your OS and FFmpeg version (`ffmpeg -version`)

## License

MIT
