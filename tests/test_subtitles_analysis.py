import pytest

from pymedia import (
    add_subtitle_track,
    convert_subtitles,
    detect_scenes,
    extract_subtitles,
    frame_accurate_trim,
    list_keyframes,
    remove_subtitle_tracks,
    trim_to_keyframes,
)


def test_convert_subtitles_srt_to_vtt():
    srt = "1\n00:00:00,000 --> 00:00:00,500\nHello\n"
    vtt = convert_subtitles(srt, src="srt", dst="vtt")
    assert vtt.startswith("WEBVTT")
    assert "00:00:00.000 --> 00:00:00.500" in vtt


def test_convert_subtitles_vtt_to_srt():
    vtt = "WEBVTT\n\n00:00:00.000 --> 00:00:00.500\nHello\n"
    srt = convert_subtitles(vtt, src="vtt", dst="srt")
    assert "00:00:00,000 --> 00:00:00,500" in srt


def test_list_keyframes(video_data):
    keys = list_keyframes(video_data)
    assert isinstance(keys, list)
    assert len(keys) >= 1
    assert keys[0] >= 0.0


def test_detect_scenes(video_data):
    scenes = detect_scenes(video_data, threshold=0.2, sample_interval=0.2)
    assert isinstance(scenes, list)


def test_trim_to_keyframes(video_data):
    clip = trim_to_keyframes(video_data, start=0.1, end=0.6)
    assert len(clip) > 0


def test_frame_accurate_trim(video_data):
    clip = frame_accurate_trim(video_data, start=0.1, end=0.6)
    assert len(clip) > 0


def test_convert_subtitles_invalid_pair():
    with pytest.raises(ValueError):
        convert_subtitles("abc", src="srt", dst="ass")


def test_add_extract_remove_subtitle_tracks(video_data):
    srt = "1\n00:00:00,000 --> 00:00:00,600\nHELLO WORLD\n"
    with_subs = add_subtitle_track(video_data, srt, lang="eng", codec="subrip")
    assert len(with_subs) > 0

    extracted = extract_subtitles(with_subs)
    assert isinstance(extracted, list)
    assert len(extracted) >= 1

    no_subs = remove_subtitle_tracks(with_subs)
    assert len(no_subs) > 0
    extracted2 = extract_subtitles(no_subs)
    assert extracted2 == []
