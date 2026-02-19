# pymedia

In-memory video processing library for Python, powered by FFmpeg. No temporary files, no subprocesses — everything runs in-process via ctypes.

```bash
pip install python-media
```

```python
from pymedia import extract_audio, get_video_info, trim_video
```

## Features

**Video operations**
- **Convert format** — remux to mp4, mkv, webm, avi, mov (fast, no re-encoding)
- **Trim video** — cut a time segment
- **Mute video** — strip all audio tracks
- **Compress video** — re-encode with H.264 at a target quality (CRF)
- **Resize video** — change resolution
- **Rotate video** — rotate 90°, 180°, or 270°
- **Change speed** — speed up or slow down playback
- **Merge videos** — concatenate two videos sequentially
- **Reverse video** — play video backwards
- **Video to GIF** — convert video (or a segment) to animated GIF

**Audio operations**
- **Extract audio** — pull audio as mp3, wav, aac, or ogg
- **Adjust volume** — increase or decrease audio volume level

**Frames**
- **Extract frame** — grab a single frame as JPEG or PNG
- **Extract frames** — grab multiple frames at a regular interval
- **Create thumbnail** — smart thumbnail from 1/3 into the video

**Metadata**
- **Get video info** — duration, resolution, codecs, fps, bitrate, etc.
- **Set metadata** — write a tag (title, artist, comment, year, …)
- **Strip metadata** — remove all metadata tags

## Installation

```bash
pip install python-media
```

No extra dependencies needed — FFmpeg is bundled inside the package.

## Usage

```python
from pymedia import (
    get_video_info,
    extract_audio, adjust_volume,
    convert_format, trim_video, mute_video,
    compress_video, resize_video, video_to_gif,
    rotate_video, change_speed, merge_videos, reverse_video,
    extract_frame, extract_frames, create_thumbnail,
    set_metadata, strip_metadata,
)

with open("video.mp4", "rb") as f:
    data = f.read()

# ── Info ──────────────────────────────────────────────────────
info = get_video_info(data)
print(info["duration"], info["width"], info["height"])

# ── Audio ─────────────────────────────────────────────────────
mp3 = extract_audio(data, format="mp3")
louder = adjust_volume(data, factor=2.0)   # double volume
quieter = adjust_volume(data, factor=0.5)  # half volume

# ── Format / basic edits ──────────────────────────────────────
webm = convert_format(data, format="webm")
clip = trim_video(data, start=0, end=10)
silent = mute_video(data)

# ── Re-encode ─────────────────────────────────────────────────
small = compress_video(data, crf=28, preset="fast")
resized = resize_video(data, width=1280)

# ── Transforms ────────────────────────────────────────────────
rotated = rotate_video(data, angle=90)          # 90, 180, 270, -90
fast = change_speed(data, speed=2.0)            # 2x faster
slow = change_speed(data, speed=0.5)            # half speed
merged = merge_videos(data, other_data)
backwards = reverse_video(data)

# ── GIF ───────────────────────────────────────────────────────
gif = video_to_gif(data, width=320, fps=10, start=0, duration=3)

# ── Frames ────────────────────────────────────────────────────
frame = extract_frame(data, timestamp=5.0, format="jpeg")
frames = extract_frames(data, interval=1.0)     # one frame per second
thumb = create_thumbnail(data)                  # frame from 1/3 in

# ── Metadata ──────────────────────────────────────────────────
tagged = set_metadata(data, key="title", value="My Video")
clean = strip_metadata(data)
```

## API reference

### Video info

```python
get_video_info(video_data) -> dict
```
Returns a dict with keys: `duration`, `width`, `height`, `fps`, `video_codec`,
`audio_codec`, `bitrate`, `sample_rate`, `channels`, `has_video`, `has_audio`, `num_streams`.

### Audio

| Function | Description |
|---|---|
| `extract_audio(data, format="mp3")` | Extract audio track. Formats: `mp3`, `wav`, `aac`, `ogg` |
| `adjust_volume(data, factor)` | Multiply audio volume. `2.0` = louder, `0.5` = quieter |

### Video transforms

| Function | Description |
|---|---|
| `convert_format(data, format)` | Remux to `mp4`, `mkv`, `webm`, `avi`, `mov`, `flv`, `ts` |
| `trim_video(data, start, end)` | Cut to time range (seconds) |
| `mute_video(data)` | Remove all audio streams |
| `compress_video(data, crf=23, preset="medium")` | Re-encode H.264. CRF 0–51, lower = better quality |
| `resize_video(data, width=-1, height=-1, crf=23)` | Resize, maintaining aspect ratio |
| `rotate_video(data, angle)` | Rotate `90`, `180`, `270`, or `-90` degrees |
| `change_speed(data, speed)` | `2.0` = 2× faster, `0.5` = half speed |
| `merge_videos(data1, data2)` | Concatenate two videos |
| `reverse_video(data)` | Play backwards (audio dropped) |
| `video_to_gif(data, fps=10, width=320, start=0, duration=-1)` | Animated GIF |

### Frames

| Function | Description |
|---|---|
| `extract_frame(data, timestamp=0.0, format="jpeg")` | Single frame as JPEG or PNG |
| `extract_frames(data, interval=1.0, format="jpeg")` | List of frames at regular intervals |
| `create_thumbnail(data, format="jpeg")` | Frame from 1/3 into the video |

### Metadata

| Function | Description |
|---|---|
| `set_metadata(data, key, value)` | Set a tag (`title`, `artist`, `comment`, `year`, …) |
| `strip_metadata(data)` | Remove all metadata tags |

## Supported formats

| Function | Formats |
|---|---|
| `extract_audio` | mp3, wav, aac, ogg |
| `convert_format` | mp4, mkv, webm, avi, mov, flv, ts |
| `extract_frame` / `extract_frames` / `create_thumbnail` | jpeg, png |
| `compress_video` / `resize_video` / `rotate_video` / `reverse_video` | H.264 mp4 output |
| `video_to_gif` | GIF |

## Platform support

| Platform | Status |
|---|---|
| Linux (x86_64) | Pre-built wheel |
| Linux (ARM64) | Pre-built wheel |
| macOS (arm64, Apple Silicon) | Pre-built wheel (requires macOS 14+) |
| macOS (x86_64, Intel) | Pre-built wheel (cross-compiled via Rosetta 2, requires macOS 14+) |
| Windows (x86_64) | Pre-built wheel (FFmpeg bundled via delvewheel) |

## Contributing

Contributions are welcome! pymedia is open source and we appreciate help from the community.

### Setting up the development environment

1. **Fork the repo** on GitHub and clone your fork:

```bash
git clone https://github.com/<your-username>/pymedia.git
cd pymedia
```

2. **Install FFmpeg dev libraries** for your platform (required only for development):

```bash
# Ubuntu / Debian
sudo apt install gcc pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev \
    libswresample-dev libswscale-dev

# macOS
brew install gcc pkg-config ffmpeg

# Windows
# Download the latest shared build from https://github.com/BtbN/FFmpeg-Builds/releases
# (pick ffmpeg-master-latest-win64-gpl-shared.zip), extract to C:\ffmpeg, and install pkg-config:
choco install pkgconfiglite -y
# Then add C:\ffmpeg\bin to your PATH and set:
# PKG_CONFIG_PATH=C:\ffmpeg\lib\pkgconfig
```

3. **Create a virtual environment and install in dev mode:**

```bash
# Linux / macOS
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
pip install pytest

# Windows (PowerShell)
python -m venv .venv
.venv\Scripts\Activate.ps1
pip install -e .
pip install pytest
```

4. **Install pre-commit hooks** (optional but recommended — runs black, isort, flake8 before every commit):

```bash
pip install pre-commit
pre-commit install
```

5. **Run the tests:**

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
   - Add the Python wrapper in the appropriate module (`audio.py`, `video.py`, `frames.py`, `transforms.py`, `metadata.py`, or `info.py`)
   - Export it from `src/pymedia/__init__.py`
   - Add tests in `tests/`

3. **Rebuild after any C changes:**

```bash
pip install -e .
```

4. **Run tests and lint:**

```bash
pytest tests/ -v
black src/ tests/
isort src/ tests/
flake8 src/ tests/
```

If you installed pre-commit hooks (step 4 of setup), formatting and lint checks run automatically on `git commit`.

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
├── __init__.py      # Public API exports
├── _core.py         # ctypes bindings (loads libpymedia.so)
├── audio.py         # extract_audio, adjust_volume
├── video.py         # convert_format, compress, resize, trim, mute, to_gif
├── transforms.py    # rotate_video, change_speed, merge_videos, reverse_video
├── frames.py        # extract_frame, extract_frames, create_thumbnail
├── metadata.py      # set_metadata, strip_metadata
├── info.py          # get_video_info
└── _lib/
    ├── pymedia.c      # All C code (FFmpeg operations)
    └── libpymedia.so  # Built automatically by `pip install` (libpymedia.dylib on macOS, pymedia.dll on Windows — not committed to git)
```

### Guidelines

- Keep Python wrappers thin — heavy lifting goes in the C code
- Every new feature needs at least one test
- Tests must not require external files — generate test data in `tests/conftest.py`
- Run `pytest tests/ -v` before submitting a PR and make sure all tests pass
- Format code with `black` and `isort` before opening a PR

### Reporting bugs

Open an issue at https://github.com/moinakmalkhan/pymedia/issues with:
- What you did
- What you expected
- What happened instead
- Your OS and Python version

## License

MIT
