import ctypes

from pymedia._core import _lib, _call_bytes_fn

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
    return _call_bytes_fn(_lib.convert_format, buf, len(video_data),
                          format.encode("utf-8"))


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
    return _call_bytes_fn(_lib.trim_video, buf, len(video_data),
                          ctypes.c_double(start), ctypes.c_double(end))


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
    return _call_bytes_fn(_lib.reencode_video, buf, len(video_data),
                          ctypes.c_int(crf), preset.encode("utf-8"),
                          ctypes.c_int(-1), ctypes.c_int(-1))


def resize_video(video_data: bytes, width: int = -1, height: int = -1,
                 crf: int = 23) -> bytes:
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
    return _call_bytes_fn(_lib.reencode_video, buf, len(video_data),
                          ctypes.c_int(crf), b"medium",
                          ctypes.c_int(width), ctypes.c_int(height))


def video_to_gif(video_data: bytes, fps: int = 10, width: int = 320,
                 start: float = 0.0, duration: float = -1.0) -> bytes:
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
    return _call_bytes_fn(_lib.video_to_gif, buf, len(video_data),
                          ctypes.c_int(fps), ctypes.c_int(width),
                          ctypes.c_double(start), ctypes.c_double(duration))
