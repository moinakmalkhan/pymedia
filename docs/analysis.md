# Analysis API

## `list_keyframes(video_data: bytes) -> list[float]`

Returns keyframe timestamps for the primary video stream.

### Detailed Description

This function reads the input media from memory and asks the native FFmpeg-backed layer for keyframe positions.  
The result is returned as a Python list of timestamps in seconds. These timestamps are useful for fast, safe cuts because stream-copy trimming is generally keyframe-boundary based.

### Parameters

- `video_data` (`bytes`): Full media file content in memory.

### Returns

- `list[float]`: Sorted keyframe timestamps in seconds.

### Errors

- Raises `RuntimeError` if keyframes cannot be extracted by the native layer.


## `detect_scenes(video_data: bytes, threshold: float = 0.35, sample_interval: float = 0.5) -> list[float]`

Detects approximate scene-change timestamps.

### Detailed Description

This function performs lightweight scene detection by sampling frames at fixed intervals, generating a compact signature per frame (JPEG size + CRC), and comparing consecutive samples.  
When the computed change score crosses `threshold`, that timestamp is marked as a scene boundary.

This method is fast and in-process, but it is approximate compared with advanced FFmpeg filter-based scene detection.

### Parameters

- `video_data` (`bytes`): Full media file content in memory.
- `threshold` (`float`, default `0.35`): Sensitivity level. Higher values produce fewer detected scenes.
- `sample_interval` (`float`, default `0.5`): Time gap in seconds between sampled frames.

### Returns

- `list[float]`: Detected scene-change timestamps in seconds (rounded to 3 decimals).

### Errors

- Raises `ValueError` if `threshold <= 0`.
- Raises `ValueError` if `sample_interval <= 0`.


## `trim_to_keyframes(video_data: bytes, start: float, end: float) -> bytes`

Trims a clip while snapping boundaries to nearby keyframes.

### Detailed Description

This function is designed for fast stream-copy style trimming with better boundary safety:

- Finds keyframes in the input.
- Chooses the nearest keyframe at or before `start`.
- Chooses the nearest keyframe at or after `end`.
- Falls back to direct trim behavior when keyframe data is unavailable.

Because it is keyframe-aligned, playback compatibility is generally better than arbitrary non-keyframe cuts.

### Parameters

- `video_data` (`bytes`): Full media file content in memory.
- `start` (`float`): Requested start timestamp in seconds. Must be `>= 0`.
- `end` (`float`): Requested end timestamp in seconds. Must be greater than `start`.

### Returns

- `bytes`: Trimmed media bytes.

### Errors

- Raises `ValueError` if `start < 0`.
- Raises `ValueError` if `end <= start`.


## `frame_accurate_trim(video_data: bytes, start: float, end: float) -> bytes`

Produces a tighter trim by cutting then re-encoding.

### Detailed Description

This function targets better boundary precision than basic keyframe-bound trimming:

1. Performs an initial trim for the requested range.
2. Re-encodes the result with H.264 (`crf=20`, `preset="medium"`) while copying audio.

The re-encode step helps produce frame-accurate boundaries, which is useful for editing workflows where exact start/end timing matters.

### Parameters

- `video_data` (`bytes`): Full media file content in memory.
- `start` (`float`): Start timestamp in seconds. Must be `>= 0`.
- `end` (`float`): End timestamp in seconds. Must be greater than `start`.

### Returns

- `bytes`: Trimmed and re-encoded clip bytes.

### Errors

- Raises `ValueError` if `start < 0`.
- Raises `ValueError` if `end <= start`.
