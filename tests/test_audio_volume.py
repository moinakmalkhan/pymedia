import pytest

from pymedia import adjust_volume, get_video_info


def test_adjust_volume_reduce(video_data):
    result = adjust_volume(video_data, factor=0.5)
    assert len(result) > 0
    info = get_video_info(result)
    assert info["has_audio"] is True
    assert info["has_video"] is True


def test_adjust_volume_amplify(video_data):
    result = adjust_volume(video_data, factor=2.0)
    assert len(result) > 0
    info = get_video_info(result)
    assert info["has_audio"] is True


def test_adjust_volume_silence(video_data):
    result = adjust_volume(video_data, factor=0.0)
    assert len(result) > 0


def test_adjust_volume_invalid(video_data):
    with pytest.raises(ValueError, match="factor must be >= 0"):
        adjust_volume(video_data, factor=-1.0)
