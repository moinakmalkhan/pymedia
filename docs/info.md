# Info API

## `get_video_info(video_data: bytes) -> dict`

Returns structured metadata and stream-level information for an input media file.

### Detailed Description

This is the primary probing function used across the library. It inspects container and stream properties and returns a JSON-safe dictionary that can be consumed by video, audio, and analysis workflows.

### Parameters

- `video_data` (`bytes`): Input media bytes.

### Returns

- `dict`: Probe result dictionary.

Typical keys include:

- `duration` (`float`): Media duration in seconds.
- `width` (`int`): Video width in pixels.
- `height` (`int`): Video height in pixels.
- `fps` (`float`): Approximate frame rate.
- `video_codec` (`str`): Video codec name.
- `audio_codec` (`str`): Audio codec name.
- `bitrate` (`int`): Container or stream bitrate.
- `sample_rate` (`int`): Audio sample rate.
- `channels` (`int`): Audio channel count.
- `has_video` (`bool`): Whether a video stream exists.
- `has_audio` (`bool`): Whether an audio stream exists.
- `num_streams` (`int`): Total stream count in the container.

### Errors

- Raises `RuntimeError` if probing fails in native layer.
