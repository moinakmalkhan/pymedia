import pytest

from pymedia import (
    change_speed,
    concat_videos,
    get_video_info,
    merge_videos,
    reverse_video,
    rotate_video,
)


def test_rotate_180(video_data):
    result = rotate_video(video_data, angle=180)
    assert len(result) > 0
    info = get_video_info(result)
    original = get_video_info(video_data)
    assert info["width"] == original["width"]
    assert info["height"] == original["height"]


def test_rotate_90(video_data):
    result = rotate_video(video_data, angle=90)
    assert len(result) > 0
    info = get_video_info(result)
    original = get_video_info(video_data)
    assert info["width"] == original["height"] or abs(info["width"] - original["height"]) <= 1
    assert info["height"] == original["width"] or abs(info["height"] - original["width"]) <= 1


def test_rotate_270(video_data):
    result = rotate_video(video_data, angle=270)
    assert len(result) > 0
    info = get_video_info(result)
    original = get_video_info(video_data)
    assert info["width"] == original["height"] or abs(info["width"] - original["height"]) <= 1


def test_rotate_invalid_angle(video_data):
    with pytest.raises(ValueError, match="Unsupported angle"):
        rotate_video(video_data, angle=45)


def test_change_speed_faster(video_data):
    result = change_speed(video_data, speed=2.0)
    assert len(result) > 0
    info = get_video_info(result)
    original = get_video_info(video_data)
    assert info["duration"] < original["duration"] + 0.1


def test_change_speed_slower(video_data):
    result = change_speed(video_data, speed=0.5)
    assert len(result) > 0
    info = get_video_info(result)
    original = get_video_info(video_data)
    assert info["duration"] > original["duration"] * 0.9


def test_change_speed_invalid(video_data):
    with pytest.raises(ValueError, match="speed must be greater than 0"):
        change_speed(video_data, speed=0)


def test_merge_videos(video_data):
    merged = merge_videos(video_data, video_data)
    assert len(merged) > 0
    info = get_video_info(merged)
    original = get_video_info(video_data)
    assert info["duration"] > original["duration"] * 1.5


def test_concat_videos(video_data):
    merged = concat_videos([video_data, video_data, video_data])
    assert len(merged) > 0
    info = get_video_info(merged)
    original = get_video_info(video_data)
    assert info["duration"] > original["duration"] * 2.2


def test_concat_videos_invalid(video_data):
    with pytest.raises(ValueError, match="at least two"):
        concat_videos([video_data])


def test_reverse_video(video_data):
    result = reverse_video(video_data)
    assert len(result) > 0
    info = get_video_info(result)
    assert info["has_video"] is True
