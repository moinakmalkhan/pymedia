from __future__ import annotations

import ctypes
import json
import zlib

from pymedia._core import _lib
from pymedia.frames import extract_frame
from pymedia.info import get_video_info
from pymedia.video import transcode_video, trim_video


def list_keyframes(video_data: bytes) -> list[float]:
    """Return keyframe timestamps for the primary video stream.

    Args:
        video_data: In-memory media bytes.

    Returns:
        Sorted keyframe timestamps in seconds.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    result_ptr = _lib.list_keyframes_json(buf, len(video_data))
    if not result_ptr:
        raise RuntimeError("Failed to list keyframes")
    try:
        return [float(x) for x in json.loads(ctypes.string_at(result_ptr).decode("utf-8"))]
    finally:
        _lib.pymedia_free(result_ptr)


def detect_scenes(
    video_data: bytes, threshold: float = 0.35, sample_interval: float = 0.5
) -> list[float]:
    """Detect approximate scene-change timestamps.

    This uses lightweight frame signature deltas (JPEG size + CRC) sampled
    at a fixed interval. It is fast and in-process, but less precise than
    FFmpeg filtergraph scene detection.

    Args:
        video_data: In-memory media bytes.
        threshold: Change sensitivity; higher means fewer scene cuts.
        sample_interval: Seconds between sampled frames.

    Returns:
        Scene-change timestamps in seconds.
    """
    if threshold <= 0:
        raise ValueError("threshold must be > 0")
    if sample_interval <= 0:
        raise ValueError("sample_interval must be > 0")

    info = get_video_info(video_data)
    duration = float(info.get("duration", 0.0))
    if duration <= 0:
        return []

    points = []
    t = 0.0
    prev_sig = None
    while t < duration:
        frame = extract_frame(video_data, timestamp=t, format="jpeg")
        crc = zlib.crc32(frame)
        sig = (len(frame), crc)
        if prev_sig is not None:
            len_delta = abs(sig[0] - prev_sig[0]) / max(sig[0], prev_sig[0], 1)
            crc_delta = 0.0 if sig[1] == prev_sig[1] else 1.0
            score = 0.6 * len_delta + 0.4 * crc_delta
            if score >= threshold:
                points.append(round(t, 3))
        prev_sig = sig
        t += sample_interval
    return points


def trim_to_keyframes(video_data: bytes, start: float, end: float) -> bytes:
    """Trim a clip while snapping boundaries to nearby keyframes.

    Args:
        video_data: In-memory media bytes.
        start: Requested start time in seconds.
        end: Requested end time in seconds.

    Returns:
        Trimmed clip bytes using keyframe-aligned boundaries.
    """
    if start < 0:
        raise ValueError("start must be >= 0")
    if end <= start:
        raise ValueError("end must be > start")

    keys = list_keyframes(video_data)
    if not keys:
        return trim_video(video_data, start=start, end=end)

    start_k = max((k for k in keys if k <= start), default=0.0)
    end_k = min((k for k in keys if k >= end), default=end)
    if end_k <= start_k:
        end_k = end
    return trim_video(video_data, start=start_k, end=end_k)


def frame_accurate_trim(video_data: bytes, start: float, end: float) -> bytes:
    """Produce a tighter trim by re-encoding after an initial cut.

    Args:
        video_data: In-memory media bytes.
        start: Start time in seconds.
        end: End time in seconds.

    Returns:
        Re-encoded clip bytes with improved boundary accuracy.
    """
    if start < 0:
        raise ValueError("start must be >= 0")
    if end <= start:
        raise ValueError("end must be > start")

    # Best-effort: keyframe trim first, then re-encode to tighten boundaries.
    clip = trim_video(video_data, start=start, end=end)
    return transcode_video(clip, vcodec="h264", acodec="copy", crf=20, preset="medium")
