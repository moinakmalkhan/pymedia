import ctypes

from pymedia._core import _lib, _call_bytes_fn

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
    return _call_bytes_fn(_lib.extract_audio, buf, len(video_data),
                          format.encode("utf-8"))
