import ctypes

from pymedia._core import _lib, _call_bytes_fn

SUPPORTED_IMAGE_FORMATS = ("jpeg", "jpg", "png")


def extract_frame(video_data: bytes, timestamp: float = 0.0,
                  format: str = "jpeg") -> bytes:
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
        raise ValueError(f"Unsupported image format '{format}'. Supported: {SUPPORTED_IMAGE_FORMATS}")

    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.extract_frame, buf, len(video_data),
                          ctypes.c_double(timestamp), fmt.encode("utf-8"))
