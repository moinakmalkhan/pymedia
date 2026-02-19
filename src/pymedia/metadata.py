import ctypes

from pymedia._core import _call_bytes_fn, _lib


def strip_metadata(video_data: bytes) -> bytes:
    """Remove all metadata tags from a video (title, artist, comment, etc.).

    Args:
        video_data: Raw video file bytes.

    Returns:
        MP4 video bytes with all metadata removed.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.strip_metadata, buf, len(video_data))


def set_metadata(video_data: bytes, key: str, value: str) -> bytes:
    """Set a metadata tag on a video (e.g. title, artist, comment).

    Existing metadata is preserved; the specified key is added or overwritten.

    Args:
        video_data: Raw video file bytes.
        key: Metadata key (e.g. "title", "artist", "comment", "year").
        value: Metadata value.

    Returns:
        MP4 video bytes with the metadata tag set.
    """
    if not key:
        raise ValueError("key must not be empty")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.set_metadata,
        buf,
        len(video_data),
        key.encode("utf-8"),
        value.encode("utf-8"),
    )
