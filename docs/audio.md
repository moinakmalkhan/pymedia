# Audio API

## `extract_audio(video_data: bytes, format: str = "mp3") -> bytes`

Extracts the audio stream from input media and returns it in a selected format.

### Detailed Description

This function takes full media bytes, decodes/demuxes the input audio stream, and writes a standalone audio output. It is useful when you need a pure audio asset from a video source.

### Parameters

- `video_data` (`bytes`): Input media file bytes.
- `format` (`str`, default `"mp3"`): Output format. Supported values: `mp3`, `wav`, `aac`, `ogg`, `flac`, `opus`.

### Returns

- `bytes`: Encoded audio file bytes in the selected format.

### Errors

- Raises `ValueError` if `format` is not supported.


## `adjust_volume(video_data: bytes, factor: float) -> bytes`

Adjusts audio loudness while preserving video stream content.

### Detailed Description

This function applies gain to decoded audio samples, then re-encodes audio and remuxes it with the original video stream. Video frames are not re-encoded by this operation.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `factor` (`float`): Gain multiplier. Example: `2.0` doubles volume, `0.5` halves volume, `0.0` mutes.

### Returns

- `bytes`: Video container bytes with adjusted audio.

### Errors

- Raises `ValueError` if `factor < 0`.


## `transcode_audio(data: bytes, format: str = "mp3", codec: str | None = None, bitrate: int | None = None, sample_rate: int | None = None, channels: int | None = None) -> bytes`

Transcodes media/audio input to a target audio output with optional encoding controls.

### Detailed Description

This is the core audio conversion function. It supports changing output format and optionally constraining codec, bitrate, sample rate, and channels. It accepts both pure audio input and media containers that include audio.

### Parameters

- `data` (`bytes`): Input media/audio bytes.
- `format` (`str`, default `"mp3"`): Output format. Supported: `mp3`, `wav`, `aac`, `ogg`, `flac`, `opus`.
- `codec` (`str | None`, default `None`): Optional explicit codec name. Must match the selected format's allowed codecs.
- `bitrate` (`int | None`, default `None`): Target bitrate in bits per second.
- `sample_rate` (`int | None`, default `None`): Target sampling rate in Hz.
- `channels` (`int | None`, default `None`): Target channel count (1 to 8).

### Returns

- `bytes`: Encoded audio bytes in the requested format.

### Errors

- Raises `ValueError` for unsupported format/codec combinations.
- Raises `ValueError` if `bitrate <= 0` when provided.
- Raises `ValueError` if `sample_rate <= 0` when provided.
- Raises `ValueError` if `channels` is outside `1..8` when provided.


## `change_audio_bitrate(data: bytes, bitrate: int, format: str = "aac") -> bytes`

Converts audio and applies a target bitrate.

### Detailed Description

This helper wraps `transcode_audio` for bitrate-focused workflows and keeps API usage simple when only bitrate control is needed.

### Parameters

- `data` (`bytes`): Input media/audio bytes.
- `bitrate` (`int`): Target bitrate in bits per second.
- `format` (`str`, default `"aac"`): Output format.

### Returns

- `bytes`: Transcoded audio bytes.

### Errors

- Raises `ValueError` if `bitrate <= 0`.


## `resample_audio(data: bytes, sample_rate: int, channels: int | None = None, format: str = "aac") -> bytes`

Resamples audio to a new sample rate and optional channel layout.

### Detailed Description

This helper focuses on temporal and channel normalization, which is useful before mixing or model ingestion pipelines.

### Parameters

- `data` (`bytes`): Input media/audio bytes.
- `sample_rate` (`int`): Target sample rate in Hz.
- `channels` (`int | None`, default `None`): Optional target channel count.
- `format` (`str`, default `"aac"`): Output format.

### Returns

- `bytes`: Resampled audio bytes.

### Errors

- Raises `ValueError` if `sample_rate <= 0`.
- Raises `ValueError` if `channels` is outside `1..8` when provided.


## `fade_audio(data: bytes, in_sec: float = 0.0, out_sec: float = 0.0) -> bytes`

Applies linear fade-in and fade-out envelopes.

### Detailed Description

The function decodes input to PCM WAV, applies sample-domain gain ramps at the beginning and end of the timeline, then returns a WAV result. It is useful for smoothing clip boundaries.

### Parameters

- `data` (`bytes`): Input media/audio bytes.
- `in_sec` (`float`, default `0.0`): Fade-in duration in seconds.
- `out_sec` (`float`, default `0.0`): Fade-out duration in seconds.

### Returns

- `bytes`: WAV bytes with fades applied.

### Errors

- Raises `ValueError` if `in_sec < 0` or `out_sec < 0`.


## `normalize_audio_lufs(data: bytes, target: float = -16.0) -> bytes`

Normalizes audio level toward a target loudness.

### Detailed Description

This implementation uses RMS-based approximation in PCM domain (not full EBU R128 integrated loudness). It computes current level, applies global gain, and returns WAV bytes.

### Parameters

- `data` (`bytes`): Input media/audio bytes.
- `target` (`float`, default `-16.0`): Target loudness in dBFS-like scale.

### Returns

- `bytes`: WAV bytes with normalized gain.


## `silence_detect(data: bytes, threshold_db: float = -40.0, min_silence: float = 0.3) -> list[dict]`

Detects silent segments from decoded audio.

### Detailed Description

The function decodes to WAV PCM, evaluates per-frame peak amplitude, and groups continuous low-amplitude regions into silence ranges.

### Parameters

- `data` (`bytes`): Input media/audio bytes.
- `threshold_db` (`float`, default `-40.0`): Silence threshold in dBFS.
- `min_silence` (`float`, default `0.3`): Minimum duration (seconds) for a region to be considered silence.

### Returns

- `list[dict]`: Silence intervals as dictionaries: `{ "start": float, "end": float }`.

### Errors

- Raises `ValueError` if `min_silence <= 0`.


## `silence_remove(data: bytes, threshold_db: float = -40.0, min_silence: float = 0.3) -> bytes`

Removes detected silence regions and compacts the timeline.

### Detailed Description

This function uses `silence_detect`, keeps non-silent spans, and writes a new compact WAV output. It is useful for podcast cleanup and speech preprocessing.

### Parameters

- `data` (`bytes`): Input media/audio bytes.
- `threshold_db` (`float`, default `-40.0`): Silence threshold in dBFS.
- `min_silence` (`float`, default `0.3`): Minimum silent duration in seconds.

### Returns

- `bytes`: WAV bytes with silent intervals removed.


## `crossfade_audio(audio_a: bytes, audio_b: bytes, duration: float) -> bytes`

Crossfades two audio inputs over an overlap duration.

### Detailed Description

The function aligns both inputs to compatible WAV properties (sample rate and channels), blends the tail of `audio_a` and head of `audio_b`, and concatenates the result.

### Parameters

- `audio_a` (`bytes`): First input media/audio bytes.
- `audio_b` (`bytes`): Second input media/audio bytes.
- `duration` (`float`): Crossfade overlap duration in seconds.

### Returns

- `bytes`: WAV bytes containing the crossfaded sequence.

### Errors

- Raises `ValueError` if `duration <= 0`.
- Raises `ValueError` if computed overlap is too small for inputs.
- Raises `ValueError` if channel/sample-rate alignment fails.


## `mix_audio_tracks(video_data: bytes, tracks: Sequence[bytes], weights: Sequence[float] | None = None, normalize: bool = True) -> bytes`

Mixes additional tracks into a video's base audio track.

### Detailed Description

The function extracts the base audio from `video_data`, decodes all tracks to aligned PCM, performs weighted sample mixing, optionally normalizes by total weight, re-encodes to AAC, and replaces original video audio.

### Parameters

- `video_data` (`bytes`): Base video bytes.
- `tracks` (`Sequence[bytes]`): Extra audio/media tracks to add.
- `weights` (`Sequence[float] | None`, default `None`): Optional per-track weights. Length must be `len(tracks) + 1` because base track is included.
- `normalize` (`bool`, default `True`): If true, divide mixed samples by total absolute weight.

### Returns

- `bytes`: Video bytes with mixed audio track.

### Errors

- Raises `ValueError` if `tracks` is empty.
- Raises `ValueError` if `weights` length is invalid.
