from __future__ import annotations

import ctypes
import io
import math
from array import array
from typing import Sequence

from pymedia._core import _call_bytes_fn, _lib

SUPPORTED_FORMATS = ("mp3", "wav", "aac", "ogg", "flac", "opus")


def extract_audio(video_data: bytes, format: str = "mp3") -> bytes:
    """Extract audio from in-memory video data.

    Args:
        video_data: Raw video file bytes.
        format: Output audio format. One of: mp3, wav, aac, ogg.

    Returns:
        Raw audio file bytes in the requested format.
    """
    if format not in SUPPORTED_FORMATS:
        raise ValueError(f"Unsupported format '{format}'. Supported: {SUPPORTED_FORMATS}")

    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.extract_audio, buf, len(video_data), format.encode("utf-8"))


def adjust_volume(video_data: bytes, factor: float) -> bytes:
    """Adjust audio volume in a video.

    The video stream is copied unchanged; audio is decoded, gain-adjusted,
    and re-encoded to AAC.

    Args:
        video_data: Raw video file bytes.
        factor: Volume multiplier. 2.0 = double, 0.5 = half, 0.0 = silence.

    Returns:
        MP4 video bytes with adjusted audio volume.
    """
    if factor < 0:
        raise ValueError("factor must be >= 0")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(_lib.adjust_volume, buf, len(video_data), ctypes.c_double(factor))


def transcode_audio(
    data: bytes,
    format: str = "mp3",
    codec: str | None = None,
    bitrate: int | None = None,
    sample_rate: int | None = None,
    channels: int | None = None,
) -> bytes:
    """Transcode audio track to a target output format."""
    if format not in SUPPORTED_FORMATS:
        raise ValueError(f"Unsupported format '{format}'. Supported: {SUPPORTED_FORMATS}")
    if codec is not None:
        # Keep validation strict until explicit codec selection is exposed.
        expected = {
            "mp3": {"libmp3lame", "mp3"},
            "aac": {"aac"},
            "ogg": {"libvorbis", "vorbis"},
            "wav": {"pcm_s16le", "wav"},
            "flac": {"flac"},
            "opus": {"libopus", "opus"},
        }[format]
        if codec not in expected:
            raise ValueError(f"Unsupported codec '{codec}' for format '{format}'")
    if bitrate is not None and bitrate <= 0:
        raise ValueError("bitrate must be > 0 when provided")
    if sample_rate is not None and sample_rate <= 0:
        raise ValueError("sample_rate must be > 0 when provided")
    if channels is not None and (channels <= 0 or channels > 8):
        raise ValueError("channels must be between 1 and 8 when provided")

    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    return _call_bytes_fn(
        _lib.transcode_audio_advanced,
        buf,
        len(data),
        format.encode("utf-8"),
        ctypes.c_int(-1 if bitrate is None else bitrate),
        ctypes.c_int(-1 if sample_rate is None else sample_rate),
        ctypes.c_int(-1 if channels is None else channels),
    )


def _decode_wav_pcm16(data: bytes) -> tuple[array, int, int]:
    """Decode PCM16 WAV bytes into sample array and stream properties.

    Returns:
        Tuple of (`samples`, `channels`, `sample_rate`).
    """
    import wave

    with wave.open(io.BytesIO(data), "rb") as wf:
        channels = wf.getnchannels()
        sample_rate = wf.getframerate()
        sampwidth = wf.getsampwidth()
        if sampwidth != 2:
            raise ValueError("Only 16-bit PCM WAV is supported")
        raw = wf.readframes(wf.getnframes())

    samples = array("h")
    samples.frombytes(raw)
    if samples.itemsize != 2:
        raise ValueError("Unexpected sample width")
    return samples, channels, sample_rate


def _decode_or_transcode_wav_pcm16(
    data: bytes, target_sample_rate: int | None = None, target_channels: int | None = None
) -> tuple[array, int, int]:
    """Decode input as WAV PCM16, extracting/transcoding only when required.

    Args:
        data: Input media/audio bytes.
        target_sample_rate: Optional target sample rate to enforce.
        target_channels: Optional target channel count to enforce.

    Returns:
        Tuple of (`samples`, `channels`, `sample_rate`) decoded from 16-bit PCM WAV.
    """
    try:
        samples, channels, sample_rate = _decode_wav_pcm16(data)
    except Exception:
        # Prefer extraction path for container inputs; it is more stable in CI.
        try:
            wav = extract_audio(data, format="wav")
            samples, channels, sample_rate = _decode_wav_pcm16(wav)
        except Exception:
            wav = transcode_audio(
                data,
                format="wav",
                sample_rate=target_sample_rate,
                channels=target_channels,
            )
            return _decode_wav_pcm16(wav)

        needs_resample = target_sample_rate is not None and sample_rate != target_sample_rate
        needs_rechannel = target_channels is not None and channels != target_channels
        if needs_resample or needs_rechannel:
            wav = transcode_audio(
                wav,
                format="wav",
                sample_rate=target_sample_rate,
                channels=target_channels,
            )
            return _decode_wav_pcm16(wav)
        return samples, channels, sample_rate

    needs_resample = target_sample_rate is not None and sample_rate != target_sample_rate
    needs_rechannel = target_channels is not None and channels != target_channels
    if needs_resample or needs_rechannel:
        wav = transcode_audio(
            data,
            format="wav",
            sample_rate=target_sample_rate,
            channels=target_channels,
        )
        return _decode_wav_pcm16(wav)
    return samples, channels, sample_rate


def _encode_wav_pcm16(samples: array, channels: int, sample_rate: int) -> bytes:
    """Encode PCM16 sample array into WAV bytes."""
    import wave

    out = io.BytesIO()
    with wave.open(out, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(samples.tobytes())
    return out.getvalue()


def change_audio_bitrate(data: bytes, bitrate: int, format: str = "aac") -> bytes:
    """Transcode audio while applying a target bitrate.

    Args:
        data: Input media/audio bytes.
        bitrate: Target bitrate in bits per second.
        format: Target output format.

    Returns:
        Transcoded audio bytes.
    """
    if bitrate <= 0:
        raise ValueError("bitrate must be > 0")
    return transcode_audio(data, format=format, bitrate=bitrate)


def resample_audio(
    data: bytes, sample_rate: int, channels: int | None = None, format: str = "aac"
) -> bytes:
    """Resample audio to a new sample rate and optional channel count."""
    if sample_rate <= 0:
        raise ValueError("sample_rate must be > 0")
    if channels is not None and (channels <= 0 or channels > 8):
        raise ValueError("channels must be between 1 and 8")
    return transcode_audio(data, format=format, sample_rate=sample_rate, channels=channels)


def fade_audio(data: bytes, in_sec: float = 0.0, out_sec: float = 0.0) -> bytes:
    """Apply linear fade-in/fade-out to decoded audio.

    Args:
        data: Input media/audio bytes.
        in_sec: Fade-in duration in seconds.
        out_sec: Fade-out duration in seconds.

    Returns:
        WAV bytes with fades applied.
    """
    if in_sec < 0 or out_sec < 0:
        raise ValueError("in_sec and out_sec must be >= 0")
    samples, channels, sample_rate = _decode_or_transcode_wav_pcm16(data)

    total_frames = len(samples) // channels
    fade_in_frames = min(int(in_sec * sample_rate), total_frames)
    fade_out_frames = min(int(out_sec * sample_rate), total_frames)

    for i in range(total_frames):
        gain = 1.0
        if fade_in_frames > 0 and i < fade_in_frames:
            gain *= i / fade_in_frames
        if fade_out_frames > 0 and i >= total_frames - fade_out_frames:
            tail_idx = i - (total_frames - fade_out_frames)
            gain *= 1.0 - (tail_idx / fade_out_frames)
        for c in range(channels):
            idx = i * channels + c
            value = int(samples[idx] * gain)
            samples[idx] = max(-32768, min(32767, value))
    return _encode_wav_pcm16(samples, channels, sample_rate)


def normalize_audio_lufs(data: bytes, target: float = -16.0) -> bytes:
    """Normalize loudness toward a target level (RMS-based approximation).

    Args:
        data: Input media/audio bytes.
        target: Target loudness in dBFS-like scale.

    Returns:
        WAV bytes with gain adjustment.
    """
    samples, channels, sample_rate = _decode_or_transcode_wav_pcm16(data)
    if len(samples) == 0:
        return _encode_wav_pcm16(samples, channels, sample_rate)

    acc = 0.0
    for s in samples:
        acc += float(s) * float(s)
    rms = math.sqrt(acc / len(samples))
    if rms <= 0:
        return _encode_wav_pcm16(samples, channels, sample_rate)

    current_dbfs = 20.0 * math.log10(rms / 32767.0)
    gain_db = target - current_dbfs
    gain = math.pow(10.0, gain_db / 20.0)

    for i in range(len(samples)):
        value = int(samples[i] * gain)
        samples[i] = max(-32768, min(32767, value))
    return _encode_wav_pcm16(samples, channels, sample_rate)


def silence_detect(
    data: bytes, threshold_db: float = -40.0, min_silence: float = 0.3
) -> list[dict]:
    """Detect silent intervals from decoded audio amplitude.

    Args:
        data: Input media/audio bytes.
        threshold_db: Silence threshold in dBFS.
        min_silence: Minimum silence duration in seconds.

    Returns:
        List of dicts with `start` and `end` second offsets.
    """
    if min_silence <= 0:
        raise ValueError("min_silence must be > 0")
    samples, channels, sample_rate = _decode_or_transcode_wav_pcm16(data)
    return _find_silence_ranges(
        samples,
        channels=channels,
        sample_rate=sample_rate,
        threshold_db=threshold_db,
        min_silence=min_silence,
    )


def silence_remove(data: bytes, threshold_db: float = -40.0, min_silence: float = 0.3) -> bytes:
    """Remove silent regions from media/audio and return compacted WAV audio.

    The input is decoded to 16-bit PCM WAV, then scanned frame-by-frame.
    Any contiguous region whose peak amplitude stays below `threshold_db` for at
    least `min_silence` seconds is removed. Non-silent regions are preserved in
    their original order without crossfades.

    Args:
        data: Input media or audio bytes.
        threshold_db: Silence threshold in dBFS. Frames at or below this level
            are considered silent.
        min_silence: Minimum contiguous silence duration (seconds) required
            before a region is removed.

    Returns:
        WAV bytes containing the input audio with detected silence removed.

    Raises:
        ValueError: If `min_silence` is not greater than zero.
    """
    if min_silence <= 0:
        raise ValueError("min_silence must be > 0")
    samples, channels, sample_rate = _decode_or_transcode_wav_pcm16(data)
    total_frames = len(samples) // channels
    if total_frames == 0:
        return _encode_wav_pcm16(samples, channels, sample_rate)

    ranges = _find_silence_ranges(
        samples,
        channels=channels,
        sample_rate=sample_rate,
        threshold_db=threshold_db,
        min_silence=min_silence,
    )
    if not ranges:
        return _encode_wav_pcm16(samples, channels, sample_rate)

    keep = array("h")
    cursor = 0
    for r in ranges:
        start_f = int(r["start"] * sample_rate)
        end_f = int(r["end"] * sample_rate)
        if start_f > cursor:
            keep.extend(samples[cursor * channels : start_f * channels])
        cursor = max(cursor, end_f)
    if cursor < total_frames:
        keep.extend(samples[cursor * channels :])
    return _encode_wav_pcm16(keep, channels, sample_rate)


def _find_silence_ranges(
    samples: array,
    *,
    channels: int,
    sample_rate: int,
    threshold_db: float,
    min_silence: float,
) -> list[dict]:
    """Scan decoded PCM samples and return silent time ranges."""
    total_frames = len(samples) // channels
    if total_frames == 0:
        return []

    threshold = int(32767 * math.pow(10.0, threshold_db / 20.0))
    min_frames = max(1, int(min_silence * sample_rate))

    silent_ranges = []
    run_start = None
    for i in range(total_frames):
        frame_peak = 0
        base = i * channels
        for c in range(channels):
            frame_peak = max(frame_peak, abs(samples[base + c]))
        is_silent = frame_peak <= threshold
        if is_silent and run_start is None:
            run_start = i
        elif not is_silent and run_start is not None:
            if i - run_start >= min_frames:
                silent_ranges.append({"start": run_start / sample_rate, "end": i / sample_rate})
            run_start = None

    if run_start is not None and total_frames - run_start >= min_frames:
        silent_ranges.append({"start": run_start / sample_rate, "end": total_frames / sample_rate})
    return silent_ranges


def crossfade_audio(audio_a: bytes, audio_b: bytes, duration: float) -> bytes:
    """Crossfade two audio inputs over a specified overlap duration.

    Inputs can be raw audio bytes or containerized media with audio streams.
    When possible, WAV PCM16 inputs are decoded directly; otherwise inputs are
    transcoded to WAV PCM16 before blending.

    Args:
        audio_a: First input audio/media bytes.
        audio_b: Second input audio/media bytes.
        duration: Crossfade duration in seconds.

    Returns:
        WAV bytes with blended transition.
    """
    if duration <= 0:
        raise ValueError("duration must be > 0")
    a_samples, a_channels, a_sr = _decode_or_transcode_wav_pcm16(audio_a)
    b_samples, b_channels, b_sr = _decode_or_transcode_wav_pcm16(
        audio_b,
        target_sample_rate=a_sr,
        target_channels=a_channels,
    )

    if a_channels != b_channels or a_sr != b_sr:
        raise ValueError("audio streams must have matching sample rate and channels")

    channels = a_channels
    sample_rate = a_sr
    a_frames = len(a_samples) // channels
    b_frames = len(b_samples) // channels
    fade_frames = min(int(duration * sample_rate), a_frames, b_frames)
    if fade_frames <= 0:
        raise ValueError("duration is too small for input audio lengths")

    out = array("h")
    out.extend(a_samples[: (a_frames - fade_frames) * channels])
    for i in range(fade_frames):
        t = i / fade_frames
        for c in range(channels):
            a_idx = (a_frames - fade_frames + i) * channels + c
            b_idx = i * channels + c
            mixed = int((1.0 - t) * a_samples[a_idx] + t * b_samples[b_idx])
            out.append(max(-32768, min(32767, mixed)))
    out.extend(b_samples[fade_frames * channels :])
    return _encode_wav_pcm16(out, channels, sample_rate)


def mix_audio_tracks(
    video_data: bytes,
    tracks: Sequence[bytes],
    weights: Sequence[float] | None = None,
    normalize: bool = True,
) -> bytes:
    """Mix additional audio tracks into a video's base audio track.

    The base track is extracted from `video_data`, each extra track is decoded
    to WAV PCM16 with matching sample rate/channel layout, and all samples are
    mixed with optional weighting. When `normalize=True`, the mixed sample is
    scaled by the sum of absolute weights to avoid excessive clipping.

    Args:
        video_data: Base video bytes.
        tracks: Extra audio/media tracks to mix in.
        weights: Optional per-track weights including base track.
        normalize: Whether to normalize by total weight.

    Returns:
        Video bytes with mixed audio track.
    """
    if not tracks:
        raise ValueError("tracks must contain at least one audio source")
    if weights is not None and len(weights) != len(tracks) + 1:
        raise ValueError("weights length must be len(tracks) + 1 (base audio + extra tracks)")

    base_samples, channels, sample_rate = _decode_or_transcode_wav_pcm16(video_data)
    tracks_decoded = [base_samples]
    decoded_cache: dict[bytes, array] = {video_data: base_samples}

    for track in tracks:
        cached = decoded_cache.get(track)
        if cached is not None:
            tracks_decoded.append(cached)
            continue
        s, _, _ = _decode_or_transcode_wav_pcm16(
            track,
            target_sample_rate=sample_rate,
            target_channels=channels,
        )
        decoded_cache[track] = s
        tracks_decoded.append(s)

    max_len = max(len(s) for s in tracks_decoded)
    if weights is None:
        weights = [1.0] * len(tracks_decoded)

    out = array("h")
    weight_sum = sum(abs(w) for w in weights) if normalize else 1.0
    if weight_sum <= 0:
        weight_sum = 1.0

    for i in range(max_len):
        mixed = 0.0
        for idx, samples in enumerate(tracks_decoded):
            if i < len(samples):
                mixed += samples[i] * weights[idx]
        mixed /= weight_sum
        out.append(max(-32768, min(32767, int(mixed))))

    mixed_wav = _encode_wav_pcm16(out, channels, sample_rate)
    mixed_aac = transcode_audio(mixed_wav, format="aac", sample_rate=sample_rate, channels=channels)

    # Late import avoids module cycle at import-time.
    from pymedia.video import replace_audio

    return replace_audio(video_data, mixed_aac, trim=True)
