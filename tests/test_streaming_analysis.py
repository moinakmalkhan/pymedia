from pymedia import (
    analyze_gop,
    analyze_loudness,
    create_fragmented_mp4,
    detect_vfr_cfr,
    package_dash,
    package_hls,
    probe_media,
    stream_copy,
)


def test_create_fragmented_mp4(video_data):
    out = create_fragmented_mp4(video_data)
    assert len(out) > 0
    assert b"moov" in out and b"moof" in out


def test_stream_copy(video_data):
    out = stream_copy(video_data, output_format="mp4")
    assert len(out) > 0


def test_probe_media(video_data):
    p = probe_media(video_data, packets=True, frames=True)
    assert "format" in p
    assert "streams" in p
    assert isinstance(p.get("packet_timestamps"), list)


def test_analyze_loudness(video_data):
    d = analyze_loudness(video_data)
    assert "rms_dbfs" in d
    assert "peak_dbfs" in d


def test_analyze_gop(video_data):
    d = analyze_gop(video_data)
    assert "keyframes" in d
    assert "gop_durations" in d


def test_detect_vfr_cfr(video_data):
    d = detect_vfr_cfr(video_data)
    assert d["mode"] in {"cfr", "vfr", "unknown"}


def test_package_hls(video_data):
    out = package_hls(video_data, segment_time=1)
    assert out["type"] == "hls"
    assert isinstance(out["master_playlist"], str)
    assert "variants" in out
    assert len(out["variants"]) >= 1
    first = out["variants"][0]
    assert isinstance(first["playlist"], str)
    assert len(first["segments"]) >= 1


def test_package_dash(video_data):
    out = package_dash(video_data, segment_time=1, profile="on-demand")
    assert out["type"] == "dash"
    assert isinstance(out["mpd"], str)
    assert len(out["segments"]) >= 1
