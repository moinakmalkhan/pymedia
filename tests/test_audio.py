import pytest

from pymedia import (
    change_audio_bitrate,
    crossfade_audio,
    extract_audio,
    fade_audio,
    normalize_audio_lufs,
    resample_audio,
    silence_detect,
    silence_remove,
    transcode_audio,
)


def test_extract_mp3(video_data):
    audio = extract_audio(video_data, format="mp3")
    assert len(audio) > 0
    assert audio[:3] == b"ID3" or audio[:2] == b"\xff\xfb"


def test_extract_wav(video_data):
    audio = extract_audio(video_data, format="wav")
    assert len(audio) > 0
    assert audio[:4] == b"RIFF"


def test_extract_aac(video_data):
    audio = extract_audio(video_data, format="aac")
    assert len(audio) > 0


def test_extract_ogg(video_data):
    audio = extract_audio(video_data, format="ogg")
    assert len(audio) > 0
    assert audio[:4] == b"OggS"


def test_unsupported_format(video_data):
    with pytest.raises(ValueError, match="Unsupported format"):
        extract_audio(video_data, format="m4a")


def test_transcode_audio_mp3(video_data):
    audio = transcode_audio(video_data, format="mp3")
    assert len(audio) > 0
    assert audio[:3] == b"ID3" or audio[:2] == b"\xff\xfb"


def test_transcode_audio_with_overrides(video_data):
    audio = transcode_audio(video_data, format="aac", bitrate=96000, sample_rate=32000, channels=1)
    assert len(audio) > 0


def test_change_audio_bitrate(video_data):
    audio = change_audio_bitrate(video_data, bitrate=96000, format="aac")
    assert len(audio) > 0


def test_resample_audio(video_data):
    audio = resample_audio(video_data, sample_rate=22050, channels=1, format="wav")
    assert len(audio) > 0
    assert audio[:4] == b"RIFF"


def test_fade_audio(video_data):
    faded = fade_audio(video_data, in_sec=0.1, out_sec=0.1)
    assert len(faded) > 0
    assert faded[:4] == b"RIFF"


def test_normalize_audio_lufs(video_data):
    normalized = normalize_audio_lufs(video_data, target=-16.0)
    assert len(normalized) > 0
    assert normalized[:4] == b"RIFF"


def test_silence_detect(video_data):
    ranges = silence_detect(video_data, threshold_db=-35.0, min_silence=0.05)
    assert isinstance(ranges, list)
    if ranges:
        assert "start" in ranges[0] and "end" in ranges[0]


def test_silence_remove(video_data):
    removed = silence_remove(video_data, threshold_db=-35.0, min_silence=0.05)
    assert len(removed) > 0
    assert removed[:4] == b"RIFF"


def test_crossfade_audio(video_data):
    wav = transcode_audio(video_data, format="wav")
    out = crossfade_audio(wav, wav, duration=0.1)
    assert len(out) > 0
    assert out[:4] == b"RIFF"


def test_invalid_input():
    with pytest.raises(RuntimeError):
        extract_audio(b"not a video", format="mp3")
