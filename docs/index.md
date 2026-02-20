# pymedia Documentation

`pymedia` is an in-memory media processing library for Python built on FFmpeg libraries through native bindings.

## Documentation Map

- [Installation](installation.md)
- [Features](features.md)
- [Video API](video.md)
- [Audio API](audio.md)
- [Frames API](frames.md)
- [Metadata API](metadata.md)
- [Info API](info.md)
- [Analysis API](analysis.md)
- [Subtitles API](subtitles.md)
- [Streaming / Packaging API](streaming.md)
- [Development Guide](development.md)

## Quick Start

```bash
python -m pip install python-media
```

```python
from pymedia import get_video_info, transcode_video, extract_audio

info = get_video_info(video_bytes)
out = transcode_video(video_bytes, vcodec="h264", acodec="copy", crf=22)
mp3 = extract_audio(video_bytes, format="mp3")
```

For build and environment details, see [Installation](installation.md).
