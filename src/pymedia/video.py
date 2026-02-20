from __future__ import annotations

import ctypes
from typing import Sequence

from pymedia._core import _call_bytes_fn, _lib
from pymedia.audio import transcode_audio
from pymedia.info import get_video_info

SUPPORTED_CONTAINER_FORMATS = ("mp4", "mkv", "webm", "avi", "mov", "flv", "ts")


def convert_format(video_data: bytes, format: str) -> bytes:
    """Convert video to a different container format (remux, no re-encoding).

    Args:
        video_data: Raw video file bytes.
        format: Target container format (mp4, mkv, webm, avi, mov, flv, ts).

    Returns:
        Video bytes in the new container format.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.convert_format, buf, len(video_data), format.encode("utf-8"))


def trim_video(video_data: bytes, start: float = 0.0, end: float = -1.0) -> bytes:
    """Trim video to a time range (remux, no re-encoding).

    Cuts to the nearest keyframe before `start`, so the actual start may be
    slightly earlier than requested.

    Args:
        video_data: Raw video file bytes.
        start: Start time in seconds.
        end: End time in seconds (-1 for end of video).

    Returns:
        Trimmed video bytes.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.trim_video, buf, len(video_data), ctypes.c_double(start), ctypes.c_double(end)
    )


def cut_video(video_data: bytes, start: float = 0.0, duration: float = -1.0) -> bytes:
    """Cut a clip from a video by start + duration.

    This is a convenience wrapper around `trim_video`.

    Args:
        video_data: Raw video file bytes.
        start: Start time in seconds.
        duration: Clip duration in seconds (-1 for until end of video).

    Returns:
        Cut video bytes.
    """
    if duration == 0:
        raise ValueError("duration must be > 0 or -1")
    end = -1.0 if duration < 0 else start + duration
    return trim_video(video_data, start=start, end=end)


def mute_video(video_data: bytes) -> bytes:
    """Remove all audio tracks from a video (remux, no re-encoding).

    Args:
        video_data: Raw video file bytes.

    Returns:
        Video bytes with audio removed.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.mute_video, buf, len(video_data))


def compress_video(video_data: bytes, crf: int = 23, preset: str = "medium") -> bytes:
    """Re-encode video with H.264 at the given CRF quality.

    Args:
        video_data: Raw video file bytes.
        crf: Constant Rate Factor (0-51). Lower = better quality, bigger file.
             Default 23 is visually lossless for most content.
        preset: Encoding speed preset. One of: ultrafast, superfast, veryfast,
                faster, fast, medium, slow, slower, veryslow.

    Returns:
        Re-encoded MP4 video bytes.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.reencode_video,
        buf,
        len(video_data),
        ctypes.c_int(crf),
        preset.encode("utf-8"),
        ctypes.c_int(-1),
        ctypes.c_int(-1),
    )


def transcode_video(
    data: bytes,
    vcodec: str = "h264",
    acodec: str = "aac",
    video_bitrate: int | None = None,
    audio_bitrate: int | None = None,
    crf: int | None = None,
    preset: str = "medium",
    hwaccel: str = "none",
) -> bytes:
    """Transcode video with codec/bitrate controls and optional audio re-encode.

    Args:
        data: Input media bytes.
        vcodec: Video codec identifier (currently H.264 only).
        acodec: Audio mode (`aac` or `copy`).
        video_bitrate: Optional target video bitrate (bps).
        audio_bitrate: Optional target audio bitrate (bps) when `acodec='aac'`.
        crf: Quality factor used when bitrate is not provided.
        preset: Encoder speed/quality preset.
        hwaccel: Hardware acceleration preference. Currently validated for
            compatibility and gracefully falls back to software.

    Returns:
        Transcoded MP4 bytes.
    """
    if hwaccel not in {"none", "vaapi", "qsv", "nvenc", "amf"}:
        raise ValueError("hwaccel must be one of: none, vaapi, qsv, nvenc, amf")
    if vcodec not in {"h264", "libx264"}:
        raise ValueError("Only H.264 video transcoding is currently supported")
    if acodec not in {"aac", "copy"}:
        raise ValueError("acodec must be 'aac' or 'copy'")

    if video_bitrate is not None and video_bitrate <= 0:
        raise ValueError("video_bitrate must be > 0 when provided")
    if audio_bitrate is not None and audio_bitrate <= 0:
        raise ValueError("audio_bitrate must be > 0 when provided")

    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    out = _call_bytes_fn(
        _lib.transcode_video_bitrate,
        buf,
        len(data),
        ctypes.c_int(-1 if video_bitrate is None else video_bitrate),
        ctypes.c_int(23 if crf is None else crf),
        preset.encode("utf-8"),
    )
    if acodec == "aac":
        aac_audio = transcode_audio(data, format="aac", bitrate=audio_bitrate)
        out = replace_audio(out, aac_audio, trim=True)
    elif audio_bitrate is not None:
        raise ValueError("audio_bitrate requires acodec='aac'")
    return out


def resize_video(video_data: bytes, width: int = -1, height: int = -1, crf: int = 23) -> bytes:
    """Resize video to the given dimensions (re-encodes with H.264).

    If only width or height is given, the other is calculated to maintain
    the aspect ratio.

    Args:
        video_data: Raw video file bytes.
        width: Target width in pixels (-1 for auto).
        height: Target height in pixels (-1 for auto).
        crf: Quality (0-51, default 23).

    Returns:
        Resized MP4 video bytes.
    """
    if width <= 0 and height <= 0:
        raise ValueError("At least one of width or height must be specified")

    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.reencode_video,
        buf,
        len(video_data),
        ctypes.c_int(crf),
        b"medium",
        ctypes.c_int(width),
        ctypes.c_int(height),
    )


def crop_video(
    video_data: bytes,
    x: int,
    y: int,
    width: int,
    height: int,
    crf: int = 23,
    preset: str = "medium",
) -> bytes:
    """Crop video to a rectangle and re-encode with H.264.

    Args:
        video_data: Raw video file bytes.
        x: Left crop offset in pixels.
        y: Top crop offset in pixels.
        width: Crop width in pixels.
        height: Crop height in pixels.
        crf: Quality (0-51, default 23).
        preset: x264 encoding preset (default "medium").

    Returns:
        Cropped MP4 video bytes.
    """
    if x < 0 or y < 0:
        raise ValueError("x and y must be >= 0")
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be > 0")

    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.crop_video,
        buf,
        len(video_data),
        ctypes.c_int(x),
        ctypes.c_int(y),
        ctypes.c_int(width),
        ctypes.c_int(height),
        ctypes.c_int(crf),
        preset.encode("utf-8"),
    )


def change_fps(video_data: bytes, fps: float, crf: int = 23, preset: str = "medium") -> bytes:
    """Convert a video to a target constant frame rate."""
    if fps <= 0:
        raise ValueError("fps must be > 0")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.change_fps,
        buf,
        len(video_data),
        ctypes.c_double(fps),
        ctypes.c_int(crf),
        preset.encode("utf-8"),
    )


def pad_video(
    video_data: bytes,
    width: int,
    height: int,
    x: int = 0,
    y: int = 0,
    color: str = "black",
    crf: int = 23,
    preset: str = "medium",
) -> bytes:
    """Pad video onto a larger canvas."""
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be > 0")
    if x < 0 or y < 0:
        raise ValueError("x and y must be >= 0")
    if color not in {"black", "white"}:
        raise ValueError("color must be 'black' or 'white'")

    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.pad_video,
        buf,
        len(video_data),
        ctypes.c_int(width),
        ctypes.c_int(height),
        ctypes.c_int(x),
        ctypes.c_int(y),
        color.encode("utf-8"),
        ctypes.c_int(crf),
        preset.encode("utf-8"),
    )


def flip_video(
    video_data: bytes,
    horizontal: bool = False,
    vertical: bool = False,
    crf: int = 23,
    preset: str = "medium",
) -> bytes:
    """Flip video horizontally and/or vertically."""
    if not horizontal and not vertical:
        raise ValueError("At least one of horizontal or vertical must be True")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.flip_video,
        buf,
        len(video_data),
        ctypes.c_int(1 if horizontal else 0),
        ctypes.c_int(1 if vertical else 0),
        ctypes.c_int(crf),
        preset.encode("utf-8"),
    )


def _apply_basic_filter(
    video_data: bytes,
    mode: int,
    p1: float,
    p2: float = 0.0,
    p3: float = 0.0,
    crf: int = 23,
    preset: str = "medium",
) -> bytes:
    """Apply a native basic filter mode to video frames."""
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.filter_video_basic,
        buf,
        len(video_data),
        ctypes.c_int(mode),
        ctypes.c_double(p1),
        ctypes.c_double(p2),
        ctypes.c_double(p3),
        ctypes.c_int(crf),
        preset.encode("utf-8"),
    )


def blur_video(
    video_data: bytes, sigma: float = 2.0, crf: int = 23, preset: str = "medium"
) -> bytes:
    """Apply blur filtering to video frames."""
    if sigma <= 0:
        raise ValueError("sigma must be > 0")
    radius = max(1.0, min(6.0, sigma))
    return _apply_basic_filter(video_data, mode=1, p1=radius, crf=crf, preset=preset)


def denoise_video(
    video_data: bytes, strength: float = 0.5, crf: int = 23, preset: str = "medium"
) -> bytes:
    """Apply lightweight denoise filtering to video frames."""
    if strength <= 0:
        raise ValueError("strength must be > 0")
    radius = 1.0 + min(5.0, strength * 5.0)
    return _apply_basic_filter(video_data, mode=2, p1=radius, crf=crf, preset=preset)


def sharpen_video(
    video_data: bytes, amount: float = 1.0, crf: int = 23, preset: str = "medium"
) -> bytes:
    """Apply unsharp-mask style sharpening to video frames."""
    if amount < 0:
        raise ValueError("amount must be >= 0")
    return _apply_basic_filter(video_data, mode=3, p1=amount, crf=crf, preset=preset)


def color_correct(
    video_data: bytes,
    brightness: float = 0.0,
    contrast: float = 1.0,
    saturation: float = 1.0,
    crf: int = 23,
    preset: str = "medium",
) -> bytes:
    """Apply brightness/contrast/saturation color correction."""
    return _apply_basic_filter(
        video_data, mode=4, p1=brightness, p2=contrast, p3=saturation, crf=crf, preset=preset
    )


def apply_lut(
    video_data: bytes, lut_file_bytes: bytes, crf: int = 23, preset: str = "medium"
) -> bytes:
    """Apply a simple LUT-like gamma transform parsed from LUT text bytes."""
    text = lut_file_bytes.decode("utf-8", errors="ignore").lower()
    gamma = 1.0
    for line in text.splitlines():
        if "gamma" in line:
            parts = line.replace("=", " ").split()
            for p in parts:
                try:
                    gamma = float(p)
                    break
                except ValueError:
                    continue
            break
    return _apply_basic_filter(video_data, mode=5, p1=gamma, crf=crf, preset=preset)


def overlay_video(
    base: bytes,
    pip: bytes,
    x: int,
    y: int,
    width: int | None = None,
    height: int | None = None,
    opacity: float = 1.0,
) -> bytes:
    """Overlay picture-in-picture media or image onto a base video.

    The current implementation uses image watermark compositing for a practical
    in-process overlay path.
    """
    from pymedia.frames import extract_frame

    is_image = pip.startswith(b"\x89PNG") or pip.startswith(b"\xff\xd8")
    if is_image:
        pip_frame = pip
    else:
        pip_source = pip
        if width is not None or height is not None:
            pip_source = resize_video(
                pip, width=-1 if width is None else width, height=-1 if height is None else height
            )
        pip_frame = extract_frame(pip_source, timestamp=0.0, format="png")
    return add_watermark(base, pip_frame, x=x, y=y, opacity=opacity)


def stack_videos(videos: Sequence[bytes], layout: str = "hstack|vstack|grid") -> bytes:
    """Compose multiple inputs into a stacked layout.

    Behavior:
    - Uses the first item in `videos` as the timeline/base stream.
    - Expands canvas to fit the chosen layout.
    - Places additional inputs by extracting a representative first frame
      from each and overlaying it into its tile position.

    This provides deterministic in-memory composition with no temporary files.
    It is intended as a practical composition helper, not a full
    stream-synchronized multi-video compositor.

    Args:
        videos: Sequence of media byte payloads. Must contain at least two
            non-empty items.
        layout: Target layout mode. Supported values:
            `hstack`, `horizontal`, `vstack`, `vertical`, `grid`.
            Legacy default value `hstack|vstack|grid` is treated as `grid`.

    Returns:
        MP4 bytes containing the composed output.

    Raises:
        ValueError: If less than two inputs are provided, any input is empty,
            layout is unsupported, or base dimensions cannot be determined.
    """
    if len(videos) < 2:
        raise ValueError("videos must contain at least two items")
    if any(not v for v in videos):
        raise ValueError("all videos must be non-empty bytes")

    from math import ceil, sqrt

    from pymedia.frames import extract_frame

    n = len(videos)
    normalized = layout.strip().lower()
    if normalized == "hstack|vstack|grid":
        normalized = "grid"

    base_info = get_video_info(videos[0])
    base_w = int(base_info.get("width", 0))
    base_h = int(base_info.get("height", 0))
    if base_w <= 0 or base_h <= 0:
        raise ValueError("Could not determine base video dimensions")

    if normalized in {"hstack", "horizontal"}:
        cols, rows = n, 1
    elif normalized in {"vstack", "vertical"}:
        cols, rows = 1, n
    elif normalized == "grid":
        cols = int(ceil(sqrt(n)))
        rows = int(ceil(n / cols))
    else:
        raise ValueError("layout must be one of: hstack, vstack, grid")

    tile_w, tile_h = base_w, base_h
    canvas_w, canvas_h = cols * tile_w, rows * tile_h
    out = pad_video(videos[0], width=canvas_w, height=canvas_h, x=0, y=0, color="black", crf=23)

    for idx, video in enumerate(videos[1:], start=1):
        row = idx // cols
        col = idx % cols
        if row >= rows:
            break
        x = col * tile_w
        y = row * tile_h
        frame = extract_frame(video, timestamp=0.0, format="png")
        out = overlay_video(out, frame, x=x, y=y, width=tile_w, height=tile_h, opacity=1.0)
    return out


def split_screen(videos: Sequence[bytes], layout: str = "2x2") -> bytes:
    """Compose videos into split-screen output.

    This is a convenience wrapper around `stack_videos` that maps common
    split-screen aliases to concrete layouts.

    Args:
        videos: Sequence of media byte payloads.
        layout: Split-screen preset. Supported values:
            `2x2`, `2x1`, `1x2`, `hstack`, `horizontal`,
            `vstack`, `vertical`, `grid`.

    Returns:
        MP4 bytes containing the composed output.

    Raises:
        ValueError: If the layout value is unsupported.
        ValueError: Propagated from `stack_videos` for invalid inputs.
    """
    normalized = layout.strip().lower()
    if normalized == "2x2":
        return stack_videos(videos, layout="grid")
    if normalized in {"1x2", "2x1"}:
        return stack_videos(videos, layout="hstack" if normalized == "2x1" else "vstack")
    if normalized in {"hstack", "horizontal", "vstack", "vertical", "grid"}:
        return stack_videos(videos, layout=normalized)
    raise ValueError("layout must be one of: 2x2, 2x1, 1x2, hstack, vstack, grid")


def apply_filtergraph(
    data: bytes,
    video_filters: Sequence[str] | str | None = None,
    audio_filters: Sequence[str] | str | None = None,
) -> bytes:
    """Apply tokenized video/audio filter pipelines in sequence.

    Supported `video_filters` tokens:
    - `blur=<sigma>`
    - `denoise=<strength>`
    - `sharpen=<amount>`
    - `brightness=<value>`
    - `contrast=<value>`
    - `saturation=<value>`

    Supported `audio_filters` tokens:
    - `volume=<factor>`
    - `normalize` or `normalize=<target_db>`
    - `fadein=<seconds>`
    - `fadeout=<seconds>`
    - `silenceremove` or `silenceremove=<threshold_db>:<min_silence_sec>`

    Filters are applied in the order provided. When both video and audio
    filters are provided, video filters run first, then audio filters.

    Args:
        data: Input media bytes.
        video_filters: Video filter token list or comma-separated token string.
        audio_filters: Audio filter token list or comma-separated token string.

    Returns:
        Media bytes with all requested filters applied.

    Raises:
        ValueError: If any video or audio token is unsupported.
        ValueError: Propagated from underlying filter APIs for invalid values.
    """
    out = data

    if video_filters:
        if isinstance(video_filters, str):
            filters = [f.strip() for f in video_filters.split(",") if f.strip()]
        else:
            filters = [f.strip() for f in video_filters if f and f.strip()]

        for f in filters:
            if f.startswith("blur="):
                out = blur_video(out, sigma=float(f.split("=", 1)[1]))
            elif f.startswith("denoise="):
                out = denoise_video(out, strength=float(f.split("=", 1)[1]))
            elif f.startswith("sharpen="):
                out = sharpen_video(out, amount=float(f.split("=", 1)[1]))
            elif f.startswith("brightness="):
                out = color_correct(out, brightness=float(f.split("=", 1)[1]))
            elif f.startswith("contrast="):
                out = color_correct(out, contrast=float(f.split("=", 1)[1]))
            elif f.startswith("saturation="):
                out = color_correct(out, saturation=float(f.split("=", 1)[1]))
            else:
                raise ValueError(f"Unsupported video filter token: {f}")

    if audio_filters:
        from pymedia.audio import (
            adjust_volume,
            fade_audio,
            normalize_audio_lufs,
            silence_remove,
            transcode_audio,
        )

        if isinstance(audio_filters, str):
            a_filters = [f.strip() for f in audio_filters.split(",") if f.strip()]
        else:
            a_filters = [f.strip() for f in audio_filters if f and f.strip()]

        for f in a_filters:
            if f.startswith("volume="):
                out = adjust_volume(out, factor=float(f.split("=", 1)[1]))
            elif f.startswith("normalize"):
                target = -16.0
                if "=" in f:
                    target = float(f.split("=", 1)[1])
                normalized = normalize_audio_lufs(out, target=target)
                normalized_aac = transcode_audio(normalized, format="aac")
                out = replace_audio(out, normalized_aac, trim=True)
            elif f.startswith("fadein="):
                fade = float(f.split("=", 1)[1])
                faded = fade_audio(out, in_sec=fade, out_sec=0.0)
                faded_aac = transcode_audio(faded, format="aac")
                out = replace_audio(out, faded_aac, trim=True)
            elif f.startswith("fadeout="):
                fade = float(f.split("=", 1)[1])
                faded = fade_audio(out, in_sec=0.0, out_sec=fade)
                faded_aac = transcode_audio(faded, format="aac")
                out = replace_audio(out, faded_aac, trim=True)
            elif f.startswith("silenceremove"):
                threshold_db = -40.0
                min_silence = 0.3
                if "=" in f:
                    raw = f.split("=", 1)[1]
                    parts = [p.strip() for p in raw.split(":")]
                    if parts and parts[0]:
                        threshold_db = float(parts[0])
                    if len(parts) > 1 and parts[1]:
                        min_silence = float(parts[1])
                compact = silence_remove(out, threshold_db=threshold_db, min_silence=min_silence)
                compact_aac = transcode_audio(compact, format="aac")
                out = replace_audio(out, compact_aac, trim=True)
            else:
                raise ValueError(f"Unsupported audio filter token: {f}")
    return out


def replace_audio(video_data: bytes, audio_source_data: bytes, trim: bool = True) -> bytes:
    """Replace a video's audio track with audio from another media file.

    The first input contributes the video stream, the second contributes
    the audio stream. Output container is MP4.

    Args:
        video_data: Media bytes containing the desired video stream.
        audio_source_data: Media bytes containing the desired audio stream.
        trim: If True, trims replacement audio to video duration.

    Returns:
        MP4 bytes with replaced audio.
    """
    video_buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    audio_buf = (ctypes.c_uint8 * len(audio_source_data)).from_buffer_copy(audio_source_data)
    return _call_bytes_fn(
        _lib.replace_audio,
        video_buf,
        len(video_data),
        audio_buf,
        len(audio_source_data),
        ctypes.c_int(1 if trim else 0),
    )


def add_watermark(
    video_data: bytes,
    watermark_image_data: bytes,
    x: int = 10,
    y: int = 10,
    opacity: float = 0.5,
    crf: int = 23,
    preset: str = "medium",
) -> bytes:
    """Overlay an image watermark onto each frame.

    Args:
        video_data: Raw video file bytes.
        watermark_image_data: Raw image/video bytes used as watermark (first frame is used).
        x: Left offset in pixels.
        y: Top offset in pixels.
        opacity: Watermark opacity from 0.0 to 1.0.
        crf: H.264 quality (0-51).
        preset: x264 preset.

    Returns:
        MP4 bytes with overlaid watermark.
    """
    if x < 0 or y < 0:
        raise ValueError("x and y must be >= 0")
    if opacity <= 0 or opacity > 1:
        raise ValueError("opacity must be in (0, 1]")

    video_buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    wm_buf = (ctypes.c_uint8 * len(watermark_image_data)).from_buffer_copy(watermark_image_data)
    return _call_bytes_fn(
        _lib.add_watermark,
        video_buf,
        len(video_data),
        wm_buf,
        len(watermark_image_data),
        ctypes.c_int(x),
        ctypes.c_int(y),
        ctypes.c_double(opacity),
        ctypes.c_int(crf),
        preset.encode("utf-8"),
    )


def video_to_gif(
    video_data: bytes, fps: int = 10, width: int = 320, start: float = 0.0, duration: float = -1.0
) -> bytes:
    """Convert video (or a segment) to an animated GIF.

    Args:
        video_data: Raw video file bytes.
        fps: Output frames per second (default 10).
        width: Output width in pixels (height auto-calculated). Default 320.
        start: Start time in seconds (default 0).
        duration: Duration in seconds (-1 for entire video).

    Returns:
        GIF file bytes.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.video_to_gif,
        buf,
        len(video_data),
        ctypes.c_int(fps),
        ctypes.c_int(width),
        ctypes.c_double(start),
        ctypes.c_double(duration),
    )


def stabilize_video(video_data: bytes, strength: int = 16) -> bytes:
    """Apply lightweight temporal stabilization.

    This implementation smooths frame-to-frame jitter while keeping audio.

    Args:
        video_data: Raw video file bytes.
        strength: Stabilization strength from 1 to 32.

    Returns:
        Stabilized MP4 bytes.
    """
    if strength <= 0:
        raise ValueError("strength must be > 0")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.stabilize_video, buf, len(video_data), ctypes.c_int(strength))


def subtitle_burn_in(
    video_data: bytes,
    subtitles: str,
    font_size: int = 24,
    margin_bottom: int = 24,
    crf: int = 23,
    preset: str = "medium",
) -> bytes:
    """Burn SRT subtitle text into video frames.

    Args:
        video_data: Raw video file bytes.
        subtitles: SRT content as text.
        font_size: Relative subtitle font size (10-48).
        margin_bottom: Bottom margin in pixels.
        crf: H.264 quality (0-51).
        preset: x264 preset.

    Returns:
        MP4 bytes with burned subtitles.
    """
    if not subtitles or not subtitles.strip():
        raise ValueError("subtitles must be non-empty SRT text")
    if font_size <= 0:
        raise ValueError("font_size must be > 0")
    if margin_bottom < 0:
        raise ValueError("margin_bottom must be >= 0")

    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.subtitle_burn_in,
        buf,
        len(video_data),
        subtitles.encode("utf-8"),
        ctypes.c_int(font_size),
        ctypes.c_int(margin_bottom),
        ctypes.c_int(crf),
        preset.encode("utf-8"),
    )


def create_audio_image_video(
    audio_data: bytes,
    images: Sequence[bytes],
    seconds_per_image: float = 2.0,
    transition: str = "fade",
    width: int = 1280,
    height: int = 720,
) -> bytes:
    """Create a slideshow video from audio + image bytes.

    Args:
        audio_data: Media bytes containing an audio stream.
        images: Image byte blobs (png/jpg/webp/etc.).
        seconds_per_image: Duration per image.
        transition: Transition type: ``fade``, ``slide_left``, or ``none``.
        width: Output width.
        height: Output height.

    Returns:
        MP4 bytes containing slideshow video + input audio.
    """
    if not images:
        raise ValueError("images must contain at least one image")
    if seconds_per_image <= 0:
        raise ValueError("seconds_per_image must be > 0")
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be > 0")
    if transition not in {"fade", "slide_left", "none"}:
        raise ValueError("transition must be one of: fade, slide_left, none")

    audio_buf = (ctypes.c_uint8 * len(audio_data)).from_buffer_copy(audio_data)
    n = len(images)
    image_ptr_type = ctypes.POINTER(ctypes.c_uint8)
    image_ptrs = (image_ptr_type * n)()
    image_sizes = (ctypes.c_size_t * n)()

    buffers = []
    for i, image in enumerate(images):
        if not image:
            raise ValueError("all images must be non-empty bytes")
        img_buf = (ctypes.c_uint8 * len(image)).from_buffer_copy(image)
        buffers.append(img_buf)
        image_ptrs[i] = ctypes.cast(img_buf, image_ptr_type)
        image_sizes[i] = len(image)

    return _call_bytes_fn(
        _lib.create_audio_image_video,
        audio_buf,
        len(audio_data),
        image_ptrs,
        image_sizes,
        ctypes.c_int(n),
        ctypes.c_double(seconds_per_image),
        transition.encode("utf-8"),
        ctypes.c_int(width),
        ctypes.c_int(height),
    )


def change_video_audio(video_data: bytes, audio_source_data: bytes, trim: bool = True) -> bytes:
    """Alias for replace_audio with identical behavior."""
    return replace_audio(video_data, audio_source_data, trim=trim)


def split_video(
    video_data: bytes,
    segment_duration: float,
    start: float = 0.0,
    end: float = -1.0,
) -> list[bytes]:
    """Split video into sequential segments of fixed duration.

    Args:
        video_data: Raw video file bytes.
        segment_duration: Segment size in seconds.
        start: Start time in seconds (default 0).
        end: End time in seconds (-1 for end of video).

    Returns:
        List of MP4 clip bytes.
    """
    if segment_duration <= 0:
        raise ValueError("segment_duration must be > 0")
    if start < 0:
        raise ValueError("start must be >= 0")
    if end != -1.0 and end <= start:
        raise ValueError("end must be > start or -1")

    info = get_video_info(video_data)
    total_duration = float(info.get("duration", 0.0))
    if total_duration <= 0:
        raise ValueError("Could not determine video duration")

    effective_end = total_duration if end < 0 else min(end, total_duration)
    if start >= effective_end:
        raise ValueError("start must be less than effective end time")

    clips: list[bytes] = []
    cursor = start
    epsilon = 1e-6
    while cursor < effective_end - epsilon:
        clip_end = min(cursor + segment_duration, effective_end)
        clips.append(trim_video(video_data, start=cursor, end=clip_end))
        cursor = clip_end
    return clips
