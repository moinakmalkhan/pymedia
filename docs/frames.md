# Frames API

## `extract_frame(video_data: bytes, timestamp: float = 0.0, format: str = "jpeg") -> bytes`

Extracts a single frame image from video at a specific timestamp.

### Detailed Description

This function decodes the input timeline until the requested time and encodes the selected frame as an image. It is useful for thumbnails, poster frames, and visual checkpoints.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `timestamp` (`float`, default `0.0`): Extraction time in seconds.
- `format` (`str`, default `"jpeg"`): Image format. Supported: `jpeg`, `jpg`, `png`.

### Returns

- `bytes`: Encoded image bytes.

### Errors

- Raises `ValueError` if `format` is unsupported.


## `extract_frames(video_data: bytes, interval: float = 1.0, format: str = "jpeg") -> list`

Extracts multiple frames at fixed time intervals.

### Detailed Description

This function reads input duration and repeatedly calls single-frame extraction from `0` to `duration` with `interval` spacing. When duration is unavailable or zero, it returns one frame at `0.0`.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `interval` (`float`, default `1.0`): Seconds between sampled frames.
- `format` (`str`, default `"jpeg"`): Image format. Supported: `jpeg`, `jpg`, `png`.

### Returns

- `list`: List of image byte payloads.

### Errors

- Raises `ValueError` if `interval <= 0`.
- Raises `ValueError` if `format` is unsupported.


## `create_thumbnail(video_data: bytes, format: str = "jpeg") -> bytes`

Creates a representative thumbnail from approximately one-third into the video.

### Detailed Description

The function obtains media duration and picks `duration / 3` as a practical default for thumbnailing, then extracts one frame in the requested format.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `format` (`str`, default `"jpeg"`): Output image format.

### Returns

- `bytes`: Thumbnail image bytes.


## `generate_preview(video_data: bytes, num_frames: int = 9, format: str = "jpeg") -> list`

Generates evenly distributed preview frames across the timeline.

### Detailed Description

This function samples endpoints and evenly spaced intermediate points to produce a preview set suitable for timeline strips and quick gallery views.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `num_frames` (`int`, default `9`): Number of preview frames to generate.
- `format` (`str`, default `"jpeg"`): Output image format. Supported: `jpeg`, `jpg`, `png`.

### Returns

- `list`: List of image byte payloads.

### Errors

- Raises `ValueError` if `num_frames <= 0`.
- Raises `ValueError` if `format` is unsupported.
