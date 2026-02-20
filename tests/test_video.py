import pytest

from pymedia import (
    add_watermark,
    change_fps,
    change_video_audio,
    compress_video,
    convert_format,
    create_audio_image_video,
    crop_video,
    cut_video,
    extract_frame,
    flip_video,
    get_video_info,
    mix_audio_tracks,
    mute_video,
    pad_video,
    replace_audio,
    resize_video,
    split_video,
    stabilize_video,
    subtitle_burn_in,
    transcode_video,
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


def test_cut_video(video_data):
    cut = cut_video(video_data, start=0.0, duration=0.5)
    assert len(cut) > 0
    info = get_video_info(cut)
    assert info["duration"] <= 0.7


def test_split_video(video_data):
    clips = split_video(video_data, segment_duration=0.4)
    assert len(clips) >= 2
    for clip in clips:
        assert len(clip) > 0
        info = get_video_info(clip)
        assert info["has_video"] is True


def test_split_video_invalid_segment(video_data):
    with pytest.raises(ValueError, match="segment_duration must be > 0"):
        split_video(video_data, segment_duration=0)


# ── Mute ──


def test_mute_video(video_data):
    muted = mute_video(video_data)
    assert len(muted) > 0

    muted_info = get_video_info(muted)
    assert muted_info["has_video"] is True
    assert muted_info["has_audio"] is False


def test_replace_audio(video_data):
    silent = mute_video(video_data)
    replaced = replace_audio(silent, video_data, trim=True)
    assert len(replaced) > 0
    info = get_video_info(replaced)
    assert info["has_video"] is True
    assert info["has_audio"] is True


def test_change_video_audio_alias(video_data):
    silent = mute_video(video_data)
    changed = change_video_audio(silent, video_data, trim=True)
    assert len(changed) > 0
    info = get_video_info(changed)
    assert info["has_video"] is True
    assert info["has_audio"] is True


def test_add_watermark(video_data):
    watermark = extract_frame(video_data, timestamp=0.0, format="png")
    watermarked = add_watermark(video_data, watermark, x=0, y=0, opacity=0.7)
    assert len(watermarked) > 0
    original = get_video_info(video_data)
    info = get_video_info(watermarked)
    assert info["width"] == original["width"]
    assert info["height"] == original["height"]


def test_add_watermark_invalid_opacity(video_data):
    watermark = extract_frame(video_data, timestamp=0.0, format="png")
    with pytest.raises(ValueError, match="opacity must be in"):
        add_watermark(video_data, watermark, opacity=0.0)


def test_stabilize_video(video_data):
    stabilized = stabilize_video(video_data, strength=8)
    assert len(stabilized) > 0
    info = get_video_info(stabilized)
    assert info["has_video"] is True


def test_subtitle_burn_in(video_data):
    srt = (
        "1\n"
        "00:00:00,000 --> 00:00:00,600\n"
        "HELLO WORLD\n\n"
        "2\n"
        "00:00:00,600 --> 00:00:01,200\n"
        "PYMEDIA TEST\n"
    )
    burned = subtitle_burn_in(video_data, srt, font_size=16, margin_bottom=8)
    assert len(burned) > 0
    info = get_video_info(burned)
    assert info["has_video"] is True


def test_create_audio_image_video(video_data):
    image1 = extract_frame(video_data, timestamp=0.0, format="png")
    image2 = extract_frame(video_data, timestamp=0.4, format="png")
    slideshow = create_audio_image_video(
        video_data,
        [image1, image2],
        seconds_per_image=0.4,
        transition="fade",
        width=64,
        height=64,
    )
    assert len(slideshow) > 0
    info = get_video_info(slideshow)
    assert info["has_video"] is True
    assert info["has_audio"] is True


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


# ── Crop ──


def test_crop_video(video_data):
    original = get_video_info(video_data)
    cropped = crop_video(video_data, x=0, y=0, width=32, height=32)
    assert len(cropped) > 0
    info = get_video_info(cropped)
    assert info["width"] == 32
    assert info["height"] == 32
    assert info["width"] <= original["width"]
    assert info["height"] <= original["height"]


def test_crop_video_invalid_dimensions(video_data):
    with pytest.raises(ValueError, match="width and height must be > 0"):
        crop_video(video_data, x=0, y=0, width=0, height=32)


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


def test_mix_audio_tracks(video_data):
    mixed = mix_audio_tracks(video_data, [video_data], normalize=True)
    assert len(mixed) > 0
    info = get_video_info(mixed)
    assert info["has_video"] is True
    assert info["has_audio"] is True


def test_transcode_video(video_data):
    out = transcode_video(video_data, vcodec="h264", acodec="copy", crf=28, preset="fast")
    assert len(out) > 0
    info = get_video_info(out)
    assert info["has_video"] is True


def test_transcode_video_with_bitrate(video_data):
    out = transcode_video(video_data, vcodec="h264", acodec="copy", video_bitrate=300000)
    assert len(out) > 0
    info = get_video_info(out)
    assert info["has_video"] is True


def test_change_fps(video_data):
    out = change_fps(video_data, fps=8)
    assert len(out) > 0
    info = get_video_info(out)
    original = get_video_info(video_data)
    assert info["has_video"] is True
    assert info["duration"] > original["duration"] * 0.5
    assert info["duration"] < original["duration"] * 1.5


def test_pad_video(video_data):
    original = get_video_info(video_data)
    out = pad_video(
        video_data, width=original["width"] + 32, height=original["height"] + 32, x=16, y=16
    )
    assert len(out) > 0
    info = get_video_info(out)
    assert info["width"] == original["width"] + 32
    assert info["height"] == original["height"] + 32


def test_flip_video(video_data):
    out = flip_video(video_data, horizontal=True)
    assert len(out) > 0
    info = get_video_info(out)
    original = get_video_info(video_data)
    assert info["width"] == original["width"]
    assert info["height"] == original["height"]
