import pytest

from pymedia import (
    compress_video,
    convert_format,
    extract_frame,
    get_video_info,
    mute_video,
    resize_video,
    trim_video,
    video_to_gif,
)

# ── Info ──


def test_get_video_info(video_data):
    info = get_video_info(video_data)
    assert isinstance(info, dict)
    assert info["has_video"] is True
    assert info["duration"] > 0
    assert info["width"] > 0
    assert info["height"] > 0
    assert "video_codec" in info
    assert "fps" in info


# ── Convert format ──


def test_convert_to_mkv(video_data):
    result = convert_format(video_data, format="matroska")
    assert len(result) > 0
    assert result[:4] == b"\x1a\x45\xdf\xa3"


# ── Trim ──


def test_trim_video(video_data):
    trimmed = trim_video(video_data, start=0, end=0.5)
    assert len(trimmed) > 0
    assert len(trimmed) < len(video_data)


# ── Mute ──


def test_mute_video(video_data):
    muted = mute_video(video_data)
    assert len(muted) > 0

    muted_info = get_video_info(muted)
    assert muted_info["has_video"] is True
    assert muted_info["has_audio"] is False


# ── Compress ──


def test_compress_video(video_data):
    compressed = compress_video(video_data, crf=35, preset="ultrafast")
    assert len(compressed) > 0


# ── Resize ──


def test_resize_video(video_data):
    resized = resize_video(video_data, width=32)
    assert len(resized) > 0

    info = get_video_info(resized)
    assert info["width"] == 32


# ── Extract frame ──


def test_extract_frame_jpeg(video_data):
    frame = extract_frame(video_data, timestamp=0.0, format="jpeg")
    assert len(frame) > 0
    assert frame[:2] == b"\xff\xd8"


def test_extract_frame_png(video_data):
    frame = extract_frame(video_data, timestamp=0.0, format="png")
    assert len(frame) > 0
    assert frame[:4] == b"\x89PNG"


def test_extract_frame_invalid_format(video_data):
    with pytest.raises(ValueError, match="Unsupported image format"):
        extract_frame(video_data, format="bmp")


# ── GIF ──


def test_video_to_gif(video_data):
    gif = video_to_gif(video_data, fps=5, width=32, start=0, duration=1)
    assert len(gif) > 0
    assert gif[:6] in (b"GIF87a", b"GIF89a")
