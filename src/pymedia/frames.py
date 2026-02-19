import ctypes

from pymedia._core import _call_bytes_fn, _lib

SUPPORTED_IMAGE_FORMATS = ("jpeg", "jpg", "png")


def extract_frame(video_data: bytes, timestamp: float = 0.0, format: str = "jpeg") -> bytes:
    """Extract a single frame from a video as an image.

    Args:
        video_data: Raw video file bytes.
        timestamp: Time in seconds to extract the frame from.
        format: Output image format (jpeg, jpg, or png).

    Returns:
        Image file bytes (JPEG or PNG).
    """
    fmt = format.lower()
    if fmt not in SUPPORTED_IMAGE_FORMATS:
        raise ValueError(
            f"Unsupported image format '{format}'. Supported: {SUPPORTED_IMAGE_FORMATS}"
        )

    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.extract_frame, buf, len(video_data), ctypes.c_double(timestamp), fmt.encode("utf-8")
    )


def extract_frames(video_data: bytes, interval: float = 1.0, format: str = "jpeg") -> list:
    """Extract multiple frames at regular time intervals.

    Args:
        video_data: Raw video file bytes.
        interval: Time between frames in seconds (default 1.0).
        format: Output image format (jpeg, jpg, or png).

    Returns:
        List of image bytes, one per sampled timestamp.
    """
    from pymedia.info import get_video_info

    if interval <= 0:
        raise ValueError("interval must be greater than 0")
    fmt = format.lower()
    if fmt not in SUPPORTED_IMAGE_FORMATS:
        raise ValueError(
            f"Unsupported image format '{format}'. Supported: {SUPPORTED_IMAGE_FORMATS}"
        )

    info = get_video_info(video_data)
    duration = info.get("duration", 0.0)
    if duration <= 0:
        return [extract_frame(video_data, timestamp=0.0, format=fmt)]

    frames = []
    ts = 0.0
    while ts < duration:
        frames.append(extract_frame(video_data, timestamp=ts, format=fmt))
        ts += interval
    return frames


def create_thumbnail(video_data: bytes, format: str = "jpeg") -> bytes:
    """Create a thumbnail by extracting a frame from 1/3 into the video.

    Args:
        video_data: Raw video file bytes.
        format: Output image format (jpeg or png).

    Returns:
        Image file bytes.
    """
    from pymedia.info import get_video_info

    info = get_video_info(video_data)
    duration = info.get("duration", 0.0)
    timestamp = duration / 3.0 if duration > 0 else 0.0
    return extract_frame(video_data, timestamp=timestamp, format=format)
