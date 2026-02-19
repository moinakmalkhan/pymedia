import pytest

from pymedia import create_thumbnail, extract_frame, extract_frames


def test_extract_frame_jpeg(video_data):
    frame = extract_frame(video_data, timestamp=0.0, format="jpeg")
    assert len(frame) > 0
    assert frame[:2] == b"\xff\xd8"  # JPEG magic bytes


def test_extract_frame_png(video_data):
    frame = extract_frame(video_data, timestamp=0.0, format="png")
    assert len(frame) > 0
    assert frame[:4] == b"\x89PNG"


def test_extract_frame_at_timestamp(video_data):
    frame = extract_frame(video_data, timestamp=0.5, format="jpeg")
    assert len(frame) > 0
    assert frame[:2] == b"\xff\xd8"


def test_extract_frame_invalid_format(video_data):
    with pytest.raises(ValueError, match="Unsupported image format"):
        extract_frame(video_data, format="bmp")


def test_extract_frames_default_interval(video_data):
    frames = extract_frames(video_data, interval=0.5)
    assert isinstance(frames, list)
    assert len(frames) >= 1
    for frame in frames:
        assert frame[:2] == b"\xff\xd8"  # JPEG magic


def test_extract_frames_png(video_data):
    frames = extract_frames(video_data, interval=0.5, format="png")
    assert len(frames) >= 1
    for frame in frames:
        assert frame[:4] == b"\x89PNG"


def test_extract_frames_invalid_interval(video_data):
    with pytest.raises(ValueError, match="interval must be greater than 0"):
        extract_frames(video_data, interval=0)


def test_extract_frames_invalid_format(video_data):
    with pytest.raises(ValueError, match="Unsupported image format"):
        extract_frames(video_data, format="bmp")


def test_create_thumbnail_jpeg(video_data):
    thumb = create_thumbnail(video_data)
    assert len(thumb) > 0
    assert thumb[:2] == b"\xff\xd8"


def test_create_thumbnail_png(video_data):
    thumb = create_thumbnail(video_data, format="png")
    assert len(thumb) > 0
    assert thumb[:4] == b"\x89PNG"
