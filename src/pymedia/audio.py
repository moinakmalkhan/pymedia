import ctypes

from pymedia._core import _call_bytes_fn, _lib

SUPPORTED_FORMATS = ("mp3", "wav", "aac", "ogg")


def extract_audio(video_data: bytes, format: str = "mp3") -> bytes:
    """Extract audio from in-memory video data.

    Args:
        video_data: Raw video file bytes.
        format: Output audio format. One of: mp3, wav, aac, ogg.

    Returns:
        Raw audio file bytes in the requested format.
    """
    if format not in SUPPORTED_FORMATS:
        raise ValueError(f"Unsupported format '{format}'. Supported: {SUPPORTED_FORMATS}")

    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.extract_audio, buf, len(video_data), format.encode("utf-8"))


def adjust_volume(video_data: bytes, factor: float) -> bytes:
    """Adjust audio volume in a video.

    The video stream is copied unchanged; audio is decoded, gain-adjusted,
    and re-encoded to AAC.

    Args:
        video_data: Raw video file bytes.
        factor: Volume multiplier. 2.0 = double, 0.5 = half, 0.0 = silence.

    Returns:
        MP4 video bytes with adjusted audio volume.
    """
    if factor < 0:
        raise ValueError("factor must be >= 0")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.adjust_volume, buf, len(video_data), ctypes.c_double(factor))
