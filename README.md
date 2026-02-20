# pymedia

In-memory media processing for Python, powered by FFmpeg through native bindings (`ctypes` + bundled/shared native library).

```bash
python -m pip install python-media
```

## Quick start

```python
from pymedia import get_video_info, transcode_video, extract_audio

with open("video.mp4", "rb") as f:
    data = f.read()

info = get_video_info(data)
out = transcode_video(data, vcodec="h264", acodec="aac", crf=22)
mp3 = extract_audio(data, format="mp3")
```

## Current status

Implemented domains:

- Video remux/transcode/edit/effects workflows
- Audio extraction/transcode/mixing/normalization workflows
- Frame extraction and preview generation
- Metadata read/write helpers
- Analysis utilities (keyframes, scene detection, trim strategies)
- Subtitle conversion and subtitle track operations
- Streaming-oriented helpers (fMP4, probe, GOP/loudness/timing analysis, HLS/DASH packaging)

Still evolving:

- More advanced stream-synchronized multi-input composition behavior
- Expanded HLS/DASH feature surface (for example encryption/richer profiles)

## Public API (grouped)

`info`
- `get_video_info`

`analysis`
- `list_keyframes`, `detect_scenes`, `trim_to_keyframes`, `frame_accurate_trim`

`audio`
- `extract_audio`, `transcode_audio`, `adjust_volume`, `fade_audio`, `normalize_audio_lufs`
- `change_audio_bitrate`, `resample_audio`, `silence_detect`, `silence_remove`
- `crossfade_audio`, `mix_audio_tracks`

`video`
- `convert_format`, `transcode_video`, `compress_video`
- `trim_video`, `cut_video`, `split_video`
- `mute_video`, `replace_audio`, `change_video_audio`
- `resize_video`, `crop_video`, `pad_video`, `change_fps`, `flip_video`
- `blur_video`, `denoise_video`, `sharpen_video`, `color_correct`, `apply_lut`, `apply_filtergraph`
- `add_watermark`, `overlay_video`, `stack_videos`, `split_screen`, `stabilize_video`
- `subtitle_burn_in`, `create_audio_image_video`, `video_to_gif`

`transforms`
- `rotate_video`, `change_speed`, `merge_videos`, `concat_videos`, `reverse_video`

`frames`
- `extract_frame`, `extract_frames`, `create_thumbnail`, `generate_preview`

`metadata`
- `set_metadata`, `strip_metadata`

`subtitles`
- `convert_subtitles`, `extract_subtitles`, `add_subtitle_track`, `remove_subtitle_tracks`

`streaming`
- `create_fragmented_mp4`, `stream_copy`, `probe_media`
- `analyze_loudness`, `analyze_gop`, `detect_vfr_cfr`
- `package_hls`, `package_dash`

For signatures and detailed behavior, see docs under `docs/`.

## Repository structure

```text
src/pymedia/
├── __init__.py           # Public API exports
├── _core.py              # Native library loading + ctypes signatures
├── video.py              # Video/remux/transcode/effects wrappers
├── audio.py              # Audio extraction/transcode/processing wrappers
├── frames.py             # Frame/thumbnail helpers
├── transforms.py         # Rotation/speed/concat helpers
├── metadata.py           # Metadata helpers
├── info.py               # Probe/info helper
├── analysis.py           # Keyframe/scene/trim analysis helpers
├── subtitles.py          # Subtitle conversion/track helpers
├── streaming.py          # fMP4/probe/packaging helpers
└── _lib/
    ├── pymedia.c         # Native entry points / bridge layer
    └── modules/          # Native C implementation split by domain
        ├── video_core.c
        ├── video_effects.c
        ├── audio.c
        ├── filters.c
        ├── transforms.c
        ├── metadata.c
        ├── subtitles_tracks.c
        └── streaming.c
```

## Installation

End user:

```bash
python -m pip install python-media
```

Development:

```bash
python -m venv .venv
source .venv/bin/activate
python -m pip install -U pip setuptools wheel
pip install -e .
pip install pytest
```

If the native library needs an explicit rebuild in source checkouts:

```bash
python setup.py build_ext --inplace
```

## Docs

- `docs/index.md`
- `docs/installation.md`
- `docs/features.md`
- `docs/video.md`
- `docs/audio.md`
- `docs/frames.md`
- `docs/metadata.md`
- `docs/info.md`
- `docs/analysis.md`
- `docs/subtitles.md`
- `docs/streaming.md`
- `docs/development.md`

## Contributing

Core rules:

- Keep wrappers in `src/pymedia/*.py` thin (validation + marshalling)
- Put heavy media logic in `src/pymedia/_lib/modules/`
- Add tests for each public API addition/behavioral branch
- Keep docs synchronized with code

Basic workflow:

```bash
pytest tests/ -v
black src/ tests/
isort src/ tests/
flake8 src/ tests/
```

## License

MIT
