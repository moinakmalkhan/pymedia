from __future__ import annotations

import ctypes
import json
import math
from typing import Any

from pymedia._core import _call_bytes_fn, _lib
from pymedia.analysis import list_keyframes
from pymedia.audio import transcode_audio
from pymedia.info import get_video_info
from pymedia.video import convert_format, split_video, transcode_video


def create_fragmented_mp4(data: bytes) -> bytes:
    """Remux media into fragmented MP4 (fMP4) output.

    Args:
        data: Input media bytes.

    Returns:
        fMP4 bytes containing `moov`/`moof` boxes.
    """
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    return _call_bytes_fn(_lib.create_fragmented_mp4, buf, len(data))


def stream_copy(data: bytes, map_spec: str | None = None, output_format: str = "mp4") -> bytes:
    """Copy streams into a new container without re-encoding.

    Args:
        data: Input media bytes.
        map_spec: Optional mapping mode; currently supports `None` or `"all"`.
        output_format: Target output container format.

    Returns:
        Remuxed media bytes.
    """
    if map_spec not in {None, "all"}:
        raise ValueError("map_spec currently supports only None or 'all'")
    return convert_format(data, format=output_format)


def _video_packet_timestamps(data: bytes) -> list[float]:
    """Return packet-level timestamps for the primary video stream."""
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    ptr = _lib.list_video_packet_timestamps_json(buf, len(data))
    if not ptr:
        raise RuntimeError("Failed to list packet timestamps")
    try:
        return [float(x) for x in json.loads(ctypes.string_at(ptr).decode("utf-8"))]
    finally:
        _lib.pymedia_free(ptr)


def probe_media(data: bytes, packets: bool = False, frames: bool = False) -> dict[str, Any]:
    """Probe media structure and optional packet/frame timestamp detail.

    Args:
        data: Input media bytes.
        packets: Include packet timestamp list when True.
        frames: Include frame timestamp estimate when True.

    Returns:
        JSON-safe dictionary with format and stream information.
    """
    info = get_video_info(data)
    out: dict[str, Any] = {
        "format": {
            "duration": info.get("duration"),
            "bitrate": info.get("bitrate"),
            "num_streams": info.get("num_streams"),
        },
        "streams": [
            (
                {
                    "type": "video",
                    "codec": info.get("video_codec"),
                    "width": info.get("width"),
                    "height": info.get("height"),
                    "fps": info.get("fps"),
                }
                if info.get("has_video")
                else None
            ),
            (
                {
                    "type": "audio",
                    "codec": info.get("audio_codec"),
                    "sample_rate": info.get("sample_rate"),
                    "channels": info.get("channels"),
                }
                if info.get("has_audio")
                else None
            ),
        ],
    }
    out["streams"] = [s for s in out["streams"] if s is not None]

    if packets or frames:
        ts = _video_packet_timestamps(data)
        out["packet_timestamps"] = ts if packets else None
        if frames:
            out["frame_timestamps_estimate"] = ts
    return out


def analyze_loudness(data: bytes) -> dict[str, float]:
    """Compute simple loudness metrics from decoded PCM audio.

    Args:
        data: Input media bytes.

    Returns:
        Dict containing RMS/peak dBFS and audio stream properties.
    """
    wav = transcode_audio(data, format="wav")
    import io
    import wave
    from array import array

    with wave.open(io.BytesIO(wav), "rb") as wf:
        raw = wf.readframes(wf.getnframes())
        sr = wf.getframerate()
        ch = wf.getnchannels()

    arr = array("h")
    arr.frombytes(raw)
    if not arr:
        return {
            "rms_dbfs": float("-inf"),
            "peak_dbfs": float("-inf"),
            "sample_rate": float(sr),
            "channels": float(ch),
        }

    acc = 0.0
    peak = 0
    for s in arr:
        v = abs(int(s))
        peak = max(peak, v)
        acc += float(s) * float(s)
    rms = math.sqrt(acc / len(arr))

    rms_dbfs = 20.0 * math.log10(max(rms, 1.0) / 32767.0)
    peak_dbfs = 20.0 * math.log10(max(peak, 1) / 32767.0)
    return {
        "rms_dbfs": float(rms_dbfs),
        "peak_dbfs": float(peak_dbfs),
        "sample_rate": float(sr),
        "channels": float(ch),
    }


def analyze_gop(data: bytes) -> dict[str, Any]:
    """Analyze GOP structure from keyframe spacing.

    Args:
        data: Input media bytes.

    Returns:
        Dict with keyframe list, GOP durations, and aggregate stats.
    """
    keys = list_keyframes(data)
    if len(keys) < 2:
        return {"keyframes": keys, "gop_durations": [], "avg_gop": 0.0, "max_gop": 0.0}

    gaps = [keys[i + 1] - keys[i] for i in range(len(keys) - 1)]
    return {
        "keyframes": keys,
        "gop_durations": gaps,
        "avg_gop": sum(gaps) / len(gaps),
        "max_gop": max(gaps),
    }


def detect_vfr_cfr(data: bytes) -> dict[str, Any]:
    """Classify stream timing as CFR or VFR from packet deltas.

    Args:
        data: Input media bytes.

    Returns:
        Dict with mode classification and jitter statistics.
    """
    ts = _video_packet_timestamps(data)
    if len(ts) < 3:
        return {"mode": "unknown", "jitter": 0.0, "packet_count": len(ts)}

    deltas = [ts[i + 1] - ts[i] for i in range(len(ts) - 1) if ts[i + 1] > ts[i]]
    if not deltas:
        return {"mode": "unknown", "jitter": 0.0, "packet_count": len(ts)}

    mean = sum(deltas) / len(deltas)
    var = sum((d - mean) ** 2 for d in deltas) / len(deltas)
    std = math.sqrt(var)
    jitter = 0.0 if mean <= 0 else std / mean
    mode = "cfr" if jitter < 0.03 else "vfr"
    return {"mode": mode, "jitter": jitter, "packet_count": len(ts), "mean_delta": mean}


def package_hls(
    data: bytes, segment_time: int = 6, variants: list[dict] | None = None, encrypt: bool = False
):
    """Package media into in-memory HLS artifacts.

    Behavior:
    - Splits input into sequential segments of `segment_time` seconds.
    - Creates one or more variants.
    - Emits a master playlist plus per-variant media playlists.
    - Emits each segment payload as bytes in the result object.

    Variant dictionary fields (optional):
    - `name` (str): Variant label used for playlist/segment names.
    - `bandwidth` (int): Advertised `BANDWIDTH` for master playlist.
    - `video_bitrate` (int): If provided, input is transcoded for that variant.

    Args:
        data: Input media bytes.
        segment_time: Target segment duration in seconds; must be > 0.
        variants: Optional list of variant config dictionaries. If omitted,
            one default variant is generated from probed bitrate.
        encrypt: Reserved flag for encryption support. Currently unsupported.

    Returns:
        Dictionary with keys:
        - `type`: `"hls"`
        - `segment_time`: int
        - `master_playlist`: str
        - `variants`: list of variant dictionaries. Each variant contains:
          `name`, `bandwidth`, `target_duration`, `playlist_name`, `playlist`,
          and `segments` (list of `{name, duration, data}`).

    Raises:
        ValueError: If `segment_time <= 0`, `encrypt` is True, or `variants`
            is explicitly provided as an empty list.
    """
    if segment_time <= 0:
        raise ValueError("segment_time must be > 0")
    if encrypt:
        raise ValueError("encrypt=True is not supported in current in-memory packaging path")

    info = get_video_info(data)
    if variants is None:
        variants = [
            {
                "name": "v0",
                "bandwidth": int(info.get("bitrate") or 1_000_000),
            }
        ]
    if not variants:
        raise ValueError("variants must contain at least one variant when provided")

    variant_entries: list[dict[str, Any]] = []
    master_lines = ["#EXTM3U", "#EXT-X-VERSION:3"]

    for idx, variant in enumerate(variants):
        name = str(variant.get("name") or f"v{idx}")
        bandwidth = int(variant.get("bandwidth") or max(1, int(info.get("bitrate") or 1_000_000)))
        playlist_name = f"{name}.m3u8"
        variant_data = data

        target_vb = variant.get("video_bitrate")
        if target_vb is not None:
            variant_data = transcode_video(
                data, vcodec="h264", acodec="copy", video_bitrate=int(target_vb), preset="medium"
            )

        clips = split_video(variant_data, segment_duration=float(segment_time))
        segment_entries: list[dict[str, Any]] = []
        playlist_lines = [
            "#EXTM3U",
            "#EXT-X-VERSION:3",
            "#EXT-X-TARGETDURATION:" + str(int(segment_time)),
        ]
        playlist_lines.append("#EXT-X-MEDIA-SEQUENCE:0")

        max_seg = 0.0
        for sidx, clip in enumerate(clips):
            seg_name = f"{name}_seg{sidx}.m4s"
            seg_bytes = create_fragmented_mp4(clip)
            seg_info = get_video_info(clip)
            seg_dur = float(seg_info.get("duration") or segment_time)
            if seg_dur <= 0:
                seg_dur = float(segment_time)
            max_seg = max(max_seg, seg_dur)
            playlist_lines.append(f"#EXTINF:{seg_dur:.3f},")
            playlist_lines.append(seg_name)
            segment_entries.append({"name": seg_name, "duration": seg_dur, "data": seg_bytes})

        playlist_lines.append("#EXT-X-ENDLIST")
        playlist = "\n".join(playlist_lines) + "\n"
        variant_entries.append(
            {
                "name": name,
                "bandwidth": bandwidth,
                "target_duration": int(max(1.0, math.ceil(max_seg or segment_time))),
                "playlist_name": playlist_name,
                "playlist": playlist,
                "segments": segment_entries,
            }
        )

        master_lines.append(f"#EXT-X-STREAM-INF:BANDWIDTH={bandwidth}")
        master_lines.append(playlist_name)

    return {
        "type": "hls",
        "segment_time": int(segment_time),
        "master_playlist": "\n".join(master_lines) + "\n",
        "variants": variant_entries,
    }


def package_dash(data: bytes, segment_time: int = 6, profile: str = "live"):
    """Package media into in-memory DASH artifacts.

    Behavior:
    - Splits input into sequential chunks.
    - Converts chunks to fragmented MP4 payloads.
    - Generates a minimal MPD string for quick integration/testing paths.

    Args:
        data: Input media bytes.
        segment_time: Target segment duration in seconds; must be > 0.
        profile: DASH profile mode. Supported: `live`, `on-demand`.

    Returns:
        Dictionary with keys:
        - `type`: `"dash"`
        - `profile`: selected profile string
        - `segment_time`: int
        - `mpd`: MPD XML text
        - `segments`: list of `{name, duration, data}` where `data` is bytes

    Raises:
        ValueError: If `segment_time <= 0` or profile is unsupported.
    """
    if segment_time <= 0:
        raise ValueError("segment_time must be > 0")
    if profile not in {"live", "on-demand"}:
        raise ValueError("profile must be 'live' or 'on-demand'")

    clips = split_video(data, segment_duration=float(segment_time))
    segments: list[dict[str, Any]] = []
    total_duration = 0.0
    for idx, clip in enumerate(clips):
        seg_name = f"chunk-{idx}.m4s"
        fmp4 = create_fragmented_mp4(clip)
        seg_info = get_video_info(clip)
        seg_dur = float(seg_info.get("duration") or segment_time)
        if seg_dur <= 0:
            seg_dur = float(segment_time)
        total_duration += seg_dur
        segments.append({"name": seg_name, "duration": seg_dur, "data": fmp4})

    mpd = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" '
        f"type=\"{'static' if profile == 'on-demand' else 'dynamic'}\" "
        f'minBufferTime="PT{max(1, int(segment_time))}S" '
        f'mediaPresentationDuration="PT{max(total_duration, 0.0):.3f}S">\n'
        '  <Period id="0">\n'
        '    <AdaptationSet mimeType="video/mp4" segmentAlignment="true">\n'
        '      <Representation id="v0" bandwidth="1000000">\n'
        "        <BaseURL>./</BaseURL>\n"
        "      </Representation>\n"
        "    </AdaptationSet>\n"
        "  </Period>\n"
        "</MPD>\n"
    )

    return {
        "type": "dash",
        "profile": profile,
        "segment_time": int(segment_time),
        "mpd": mpd,
        "segments": segments,
    }
