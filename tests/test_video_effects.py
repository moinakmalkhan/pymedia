from pymedia import (
    apply_filtergraph,
    apply_lut,
    blur_video,
    color_correct,
    denoise_video,
    extract_frame,
    get_video_info,
    overlay_video,
    sharpen_video,
    split_screen,
    stack_videos,
)


def test_blur_video(video_data):
    out = blur_video(video_data, sigma=2.0)
    assert len(out) > 0
    info = get_video_info(out)
    assert info["has_video"] is True


def test_denoise_video(video_data):
    out = denoise_video(video_data, strength=0.7)
    assert len(out) > 0


def test_sharpen_video(video_data):
    out = sharpen_video(video_data, amount=1.2)
    assert len(out) > 0


def test_color_correct(video_data):
    out = color_correct(video_data, brightness=0.05, contrast=1.1, saturation=1.1)
    assert len(out) > 0


def test_apply_lut(video_data):
    lut = b"# simple\ngamma=1.2\n"
    out = apply_lut(video_data, lut)
    assert len(out) > 0


def test_overlay_video(video_data):
    pip = extract_frame(video_data, timestamp=0.0, format="png")
    out = overlay_video(video_data, pip, x=4, y=4, width=24, height=24, opacity=0.8)
    assert len(out) > 0


def test_apply_filtergraph(video_data):
    out = apply_filtergraph(video_data, video_filters=["blur=2", "sharpen=1.1", "brightness=0.02"])
    assert len(out) > 0


def test_apply_filtergraph_audio(video_data):
    out = apply_filtergraph(video_data, audio_filters=["volume=1.1", "fadeout=0.1"])
    assert len(out) > 0


def test_stack_videos(video_data):
    out = stack_videos([video_data, video_data], layout="hstack")
    assert len(out) > 0
    info = get_video_info(out)
    assert info["has_video"] is True


def test_split_screen(video_data):
    out = split_screen([video_data, video_data], layout="2x2")
    assert len(out) > 0
