import ctypes
import json

from pymedia._core import _lib


def get_video_info(video_data: bytes) -> dict:
    """Get metadata/info about a video file.

    Args:
        video_data: Raw video file bytes.

    Returns:
        Dict with keys like: duration, width, height, fps, video_codec,
        audio_codec, bitrate, sample_rate, channels, has_video, has_audio,
        num_streams.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    result_ptr = _lib.get_video_info(buf, len(video_data))
    if not result_ptr:
        raise RuntimeError("Failed to get video info")
    try:
        return json.loads(ctypes.string_at(result_ptr).decode("utf-8"))
    finally:
        _lib.pymedia_free(result_ptr)
