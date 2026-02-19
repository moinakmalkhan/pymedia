import ctypes

from pymedia._core import _call_bytes_fn, _lib

SUPPORTED_ANGLES = (90, 180, 270, -90)


def rotate_video(video_data: bytes, angle: int) -> bytes:
    """Rotate video by 90, 180, or 270 degrees (re-encodes with H.264).

    Args:
        video_data: Raw video file bytes.
        angle: Rotation angle in degrees. One of: 90, 180, 270, -90.

    Returns:
        Rotated MP4 video bytes.
    """
    if angle not in SUPPORTED_ANGLES:
        raise ValueError(f"Unsupported angle '{angle}'. Supported: {SUPPORTED_ANGLES}")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.rotate_video, buf, len(video_data), ctypes.c_int(angle))


def change_speed(video_data: bytes, speed: float) -> bytes:
    """Change video playback speed by rescaling timestamps.

    Note: This adjusts both video and audio timing. Audio pitch will change
    proportionally (tape-speed effect). Use speed > 1.0 to speed up, < 1.0
    to slow down.

    Args:
        video_data: Raw video file bytes.
        speed: Speed multiplier. 2.0 = 2x faster, 0.5 = half speed.

    Returns:
        Speed-adjusted MP4 video bytes.
    """
    if speed <= 0:
        raise ValueError("speed must be greater than 0")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.change_speed, buf, len(video_data), ctypes.c_double(speed))


def merge_videos(video_data1: bytes, video_data2: bytes) -> bytes:
    """Concatenate two videos sequentially.

    Both videos should have compatible codecs and resolution for best results.
    The second video is appended immediately after the first.

    Args:
        video_data1: First video bytes.
        video_data2: Second video bytes.

    Returns:
        Merged MP4 video bytes.
    """
    buf1 = (ctypes.c_uint8 * len(video_data1)).from_buffer_copy(video_data1)
    buf2 = (ctypes.c_uint8 * len(video_data2)).from_buffer_copy(video_data2)
    return _call_bytes_fn(
        _lib.merge_videos,
        buf1,
        len(video_data1),
        buf2,
        len(video_data2),
    )


def reverse_video(video_data: bytes) -> bytes:
    """Reverse video playback (plays backwards).

    Decodes all frames into memory and re-encodes in reverse order.
    Audio is dropped. Memory usage scales with video length.

    Args:
        video_data: Raw video file bytes.

    Returns:
        Reversed MP4 video bytes (no audio).
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.reverse_video, buf, len(video_data))
