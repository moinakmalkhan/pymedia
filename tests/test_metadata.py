import pytest

from pymedia import get_video_info, set_metadata, strip_metadata


def test_strip_metadata(video_data):
    result = strip_metadata(video_data)
    assert len(result) > 0
    info = get_video_info(result)
    assert info["has_video"] is True


def test_set_metadata_title(video_data):
    result = set_metadata(video_data, key="title", value="Test Video")
    assert len(result) > 0
    info = get_video_info(result)
    assert info["has_video"] is True


def test_set_metadata_artist(video_data):
    result = set_metadata(video_data, key="artist", value="Test Artist")
    assert len(result) > 0


def test_set_metadata_empty_key(video_data):
    with pytest.raises(ValueError, match="key must not be empty"):
        set_metadata(video_data, key="", value="value")
