# Streaming / Packaging API

## `create_fragmented_mp4(data: bytes) -> bytes`

Creates fragmented MP4 (fMP4) output from input media.

### Detailed Description

This function remuxes media into fMP4 layout suitable for streaming workflows. Output contains fragment-oriented boxes (for example `moof`) in addition to initialization metadata.

### Parameters

- `data` (`bytes`): Input media bytes.

### Returns

- `bytes`: Fragmented MP4 bytes.


## `stream_copy(data: bytes, map_spec: str | None = None, output_format: str = "mp4") -> bytes`

Copies streams into a new container without re-encoding.

### Detailed Description

This is a remux operation. It preserves encoded stream payloads and changes container wrapping only, which is faster and avoids generation loss.

### Parameters

- `data` (`bytes`): Input media bytes.
- `map_spec` (`str | None`, default `None`): Mapping behavior. Currently supported: `None`, `"all"`.
- `output_format` (`str`, default `"mp4"`): Target container format supported by convert/remux path.

### Returns

- `bytes`: Remuxed media bytes.

### Errors

- Raises `ValueError` if `map_spec` is unsupported.


## `probe_media(data: bytes, packets: bool = False, frames: bool = False) -> dict[str, Any]`

Probes media structure with optional packet/frame timing detail.

### Detailed Description

This function builds a JSON-safe report containing format-level fields and stream descriptors. Optional flags add packet timestamp arrays and frame timestamp estimates.

### Parameters

- `data` (`bytes`): Input media bytes.
- `packets` (`bool`, default `False`): Include packet timestamp list.
- `frames` (`bool`, default `False`): Include frame timestamp estimate.

### Returns

- `dict[str, Any]`: Probe report with keys such as `format`, `streams`, and optional timestamp arrays.


## `analyze_loudness(data: bytes) -> dict[str, float]`

Computes basic loudness metrics from decoded audio.

### Detailed Description

The function decodes audio to PCM and computes RMS and peak levels in dBFS scale, along with sample-rate/channel metadata.

### Parameters

- `data` (`bytes`): Input media bytes.

### Returns

- `dict[str, float]`: Metrics including `rms_dbfs`, `peak_dbfs`, `sample_rate`, and `channels`.


## `analyze_gop(data: bytes) -> dict[str, Any]`

Analyzes GOP timing from keyframe spacing.

### Detailed Description

This function uses keyframe timestamps to calculate GOP duration sequence and aggregate stats such as average and maximum GOP interval.

### Parameters

- `data` (`bytes`): Input media bytes.

### Returns

- `dict[str, Any]`: Includes `keyframes`, `gop_durations`, `avg_gop`, and `max_gop`.


## `detect_vfr_cfr(data: bytes) -> dict[str, Any]`

Classifies timing behavior as CFR or VFR from packet timestamp jitter.

### Detailed Description

The function computes inter-packet deltas and estimates jitter (`std/mean`). Low jitter is classified as CFR, otherwise VFR.

### Parameters

- `data` (`bytes`): Input media bytes.

### Returns

- `dict[str, Any]`: Includes `mode`, `jitter`, `packet_count`, and `mean_delta` when available.


## `package_hls(data: bytes, segment_time: int = 6, variants: list[dict] | None = None, encrypt: bool = False)`

Packages media into HLS outputs.

### Detailed Description

Generates in-memory HLS packaging output containing:

- master playlist text
- per-variant media playlist text
- segment payloads as bytes (`.m4s` fragmented MP4 segments)

### Parameters

- `data` (`bytes`): Input media bytes.
- `segment_time` (`int`, default `6`): Target segment duration in seconds.
- `variants` (`list[dict] | None`, default `None`): Variant profile definitions.
- `encrypt` (`bool`, default `False`): Enable encrypted packaging behavior.

### Returns

- `dict[str, Any]`: Packaging result with `type`, `master_playlist`, and `variants`.

Result shape:

- `type` (`str`): `"hls"`
- `segment_time` (`int`)
- `master_playlist` (`str`)
- `variants` (`list[dict]`), where each variant includes:
  `name`, `bandwidth`, `target_duration`, `playlist_name`, `playlist`, `segments`
- `segments` is `list[dict]` with fields:
  `name` (`str`), `duration` (`float`), `data` (`bytes`)

### Errors

- Raises `ValueError` if `segment_time <= 0`.
- Raises `ValueError` when `encrypt=True` (not supported in current in-memory path).


## `package_dash(data: bytes, segment_time: int = 6, profile: str = "live")`

Packages media into DASH outputs.

### Detailed Description

Generates in-memory DASH output containing:

- MPD XML text
- fragmented MP4 segment payloads as bytes (`.m4s`)

### Parameters

- `data` (`bytes`): Input media bytes.
- `segment_time` (`int`, default `6`): Target segment duration in seconds.
- `profile` (`str`, default `"live"`): DASH profile selector.

### Returns

- `dict[str, Any]`: Packaging result with `type`, `profile`, `mpd`, and `segments`.

Result shape:

- `type` (`str`): `"dash"`
- `profile` (`str`)
- `segment_time` (`int`)
- `mpd` (`str`)
- `segments` (`list[dict]`) with fields:
  `name` (`str`), `duration` (`float`), `data` (`bytes`)

### Errors

- Raises `ValueError` if `segment_time <= 0`.
- Raises `ValueError` for unsupported profile values.
