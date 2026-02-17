import pytest

from pymedia import extract_audio


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
        extract_audio(video_data, format="flac")


def test_invalid_input():
    with pytest.raises(RuntimeError):
        extract_audio(b"not a video", format="mp3")
