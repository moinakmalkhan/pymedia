# Video API

## `convert_format(video_data: bytes, format: str) -> bytes`

Remuxes media into a different container without re-encoding streams.

### Detailed Description

Use this for fast container conversion when codecs are already acceptable. Stream payloads are copied and wrapped into a new container format.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `format` (`str`): Target container. Supported: `mp4`, `mkv`, `webm`, `avi`, `mov`, `flv`, `ts`.

### Returns

- `bytes`: Remuxed media bytes.


## `transcode_video(data: bytes, vcodec: str = "h264", acodec: str = "aac", video_bitrate: int | None = None, audio_bitrate: int | None = None, crf: int | None = None, preset: str = "medium", hwaccel: str = "none") -> bytes`

Re-encodes video to H.264 with configurable quality/bitrate behavior.

### Detailed Description

This is the main controlled transcode path. Video is encoded as H.264 MP4. Audio can be copied or re-encoded to AAC. Bitrate mode and CRF mode are both supported.

### Parameters

- `data` (`bytes`): Input media bytes.
- `vcodec` (`str`, default `"h264"`): Currently supports `h264` / `libx264`.
- `acodec` (`str`, default `"aac"`): `aac` to re-encode audio, `copy` to keep source audio.
- `video_bitrate` (`int | None`): Target video bitrate in bps.
- `audio_bitrate` (`int | None`): Target AAC bitrate in bps when `acodec="aac"`.
- `crf` (`int | None`): Quality control when `video_bitrate` is not set.
- `preset` (`str`, default `"medium"`): x264 speed/efficiency preset.
- `hwaccel` (`str`, default `"none"`): Hardware preference (`none`, `vaapi`, `qsv`, `nvenc`, `amf`) currently validated for compatibility.

### Returns

- `bytes`: Transcoded MP4 bytes.

### Errors

- Raises `ValueError` for unsupported codec or hardware mode values.
- Raises `ValueError` for invalid bitrate values.
- Raises `ValueError` if `audio_bitrate` is given while `acodec="copy"`.


## `compress_video(video_data: bytes, crf: int = 23, preset: str = "medium") -> bytes`

Re-encodes input to H.264 MP4 using CRF quality mode.

### Detailed Description

This is a simplified convenience API for size/quality tradeoff without explicit bitrate tuning.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `crf` (`int`, default `23`): Lower value means better quality and larger output.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Re-encoded MP4 bytes.


## `trim_video(video_data: bytes, start: float = 0.0, end: float = -1.0) -> bytes`

Cuts a time range from media.

### Detailed Description

Trim operation is optimized for stream-copy style behavior and therefore aligns to keyframe boundaries where needed.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `start` (`float`, default `0.0`): Start time in seconds.
- `end` (`float`, default `-1.0`): End time in seconds, `-1.0` means until media end.

### Returns

- `bytes`: Trimmed media bytes.


## `cut_video(video_data: bytes, start: float = 0.0, duration: float = -1.0) -> bytes`

Convenience wrapper around trim using start + duration.

### Detailed Description

Computes `end = start + duration` (unless duration is negative) and delegates to `trim_video`.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `start` (`float`, default `0.0`): Start time.
- `duration` (`float`, default `-1.0`): Clip duration. Negative means until end.

### Returns

- `bytes`: Cut media bytes.

### Errors

- Raises `ValueError` if `duration == 0`.


## `split_video(video_data: bytes, segment_duration: float, start: float = 0.0, end: float = -1.0) -> list[bytes]`

Splits media into sequential fixed-length clips.

### Detailed Description

The function probes duration, then repeatedly trims segments until end boundary is reached.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `segment_duration` (`float`): Segment length in seconds.
- `start` (`float`, default `0.0`): Start time.
- `end` (`float`, default `-1.0`): Stop time, `-1.0` means media end.

### Returns

- `list[bytes]`: Ordered list of segment payloads.

### Errors

- Raises `ValueError` for invalid ranges or unknown input duration.


## `mute_video(video_data: bytes) -> bytes`

Removes all audio streams from input media.

### Detailed Description

This keeps video data and outputs a silent version with audio tracks dropped.

### Parameters

- `video_data` (`bytes`): Input media bytes.

### Returns

- `bytes`: Video-only output bytes.


## `replace_audio(video_data: bytes, audio_source_data: bytes, trim: bool = True) -> bytes`

Replaces a video's audio stream with audio from another source.

### Detailed Description

Video comes from `video_data`, audio comes from `audio_source_data`. Useful for dubbing, voice replacement, and soundtrack swap workflows.

### Parameters

- `video_data` (`bytes`): Source of video stream.
- `audio_source_data` (`bytes`): Source of replacement audio stream.
- `trim` (`bool`, default `True`): Trim replacement audio to video duration.

### Returns

- `bytes`: MP4 bytes with replaced audio.


## `change_video_audio(video_data: bytes, audio_source_data: bytes, trim: bool = True) -> bytes`

Alias of `replace_audio`.

### Detailed Description

Provided for naming convenience; behavior is identical to `replace_audio`.

### Parameters

- `video_data` (`bytes`): Source video media.
- `audio_source_data` (`bytes`): Replacement audio media.
- `trim` (`bool`, default `True`): Trim behavior for replacement audio.

### Returns

- `bytes`: MP4 bytes with replaced audio.


## `resize_video(video_data: bytes, width: int = -1, height: int = -1, crf: int = 23) -> bytes`

Resizes video dimensions and re-encodes output.

### Detailed Description

If only one dimension is provided, aspect ratio is preserved by auto-calculating the other dimension.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `width` (`int`, default `-1`): Target width or auto value.
- `height` (`int`, default `-1`): Target height or auto value.
- `crf` (`int`, default `23`): Re-encode quality value.

### Returns

- `bytes`: Resized MP4 bytes.

### Errors

- Raises `ValueError` if both width and height are unspecified/invalid.


## `crop_video(video_data: bytes, x: int, y: int, width: int, height: int, crf: int = 23, preset: str = "medium") -> bytes`

Crops a rectangular region from video and re-encodes output.

### Detailed Description

Coordinates specify top-left origin and target region size. Output dimensions equal crop dimensions.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `x` (`int`): Left offset in pixels.
- `y` (`int`): Top offset in pixels.
- `width` (`int`): Crop width.
- `height` (`int`): Crop height.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Cropped MP4 bytes.

### Errors

- Raises `ValueError` for invalid coordinates or sizes.


## `pad_video(video_data: bytes, width: int, height: int, x: int = 0, y: int = 0, color: str = "black", crf: int = 23, preset: str = "medium") -> bytes`

Places video onto a larger canvas.

### Detailed Description

Padding is useful for letterboxing and standardizing output dimensions across mixed sources.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `width` (`int`): Output canvas width.
- `height` (`int`): Output canvas height.
- `x` (`int`, default `0`): Horizontal offset of original frame in canvas.
- `y` (`int`, default `0`): Vertical offset of original frame in canvas.
- `color` (`str`, default `"black"`): Background color (`black` or `white`).
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Padded MP4 bytes.

### Errors

- Raises `ValueError` for invalid dimensions, offsets, or color.


## `flip_video(video_data: bytes, horizontal: bool = False, vertical: bool = False, crf: int = 23, preset: str = "medium") -> bytes`

Flips video horizontally and/or vertically.

### Detailed Description

At least one direction must be enabled. Useful for camera orientation fixes and mirrored outputs.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `horizontal` (`bool`, default `False`): Apply horizontal mirror.
- `vertical` (`bool`, default `False`): Apply vertical flip.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Flipped MP4 bytes.

### Errors

- Raises `ValueError` if both directions are `False`.


## `change_fps(video_data: bytes, fps: float, crf: int = 23, preset: str = "medium") -> bytes`

Converts input to a target constant frame rate.

### Detailed Description

This operation re-encodes video and is useful for CFR normalization in editing or model pipelines.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `fps` (`float`): Target frame rate.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: FPS-normalized MP4 bytes.

### Errors

- Raises `ValueError` if `fps <= 0`.


## `rotate_video(video_data: bytes, angle: int) -> bytes`

Rotates video by fixed supported angles.

### Detailed Description

Current accepted angles are `90`, `180`, `270`, and `-90`.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `angle` (`int`): Rotation angle.

### Returns

- `bytes`: Rotated MP4 bytes.

### Errors

- Raises `ValueError` for unsupported angles.


## `change_speed(video_data: bytes, speed: float) -> bytes`

Adjusts playback speed for video and audio.

### Detailed Description

`speed > 1` accelerates playback, `speed < 1` slows it down. Audio timing is also rescaled.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `speed` (`float`): Playback speed multiplier.

### Returns

- `bytes`: Speed-adjusted MP4 bytes.

### Errors

- Raises `ValueError` if `speed <= 0`.


## `merge_videos(video_data1: bytes, video_data2: bytes) -> bytes`

Concatenates two videos in sequence.

### Detailed Description

Second input starts immediately after first. Best results occur when both inputs have compatible encoding properties.

### Parameters

- `video_data1` (`bytes`): First clip.
- `video_data2` (`bytes`): Second clip.

### Returns

- `bytes`: Concatenated MP4 bytes.


## `concat_videos(videos: Sequence[bytes]) -> bytes`

Concatenates multiple clips in order.

### Detailed Description

Internally chains pairwise merges. Use this when joining more than two clips.

### Parameters

- `videos` (`Sequence[bytes]`): Ordered input clips.

### Returns

- `bytes`: Concatenated MP4 bytes.

### Errors

- Raises `ValueError` if fewer than two clips are provided.
- Raises `ValueError` if any clip is empty.


## `reverse_video(video_data: bytes) -> bytes`

Reverses frame order for video playback.

### Detailed Description

Video frames are decoded and re-encoded in reverse order. Current behavior drops audio.

### Parameters

- `video_data` (`bytes`): Input media bytes.

### Returns

- `bytes`: Reversed MP4 bytes (without audio).


## `add_watermark(video_data: bytes, watermark_image_data: bytes, x: int = 10, y: int = 10, opacity: float = 0.5, crf: int = 23, preset: str = "medium") -> bytes`

Overlays watermark image onto all frames.

### Detailed Description

Useful for branding, ownership marking, and visual labeling. Watermark position and alpha are configurable.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `watermark_image_data` (`bytes`): Image (or media where first frame is used).
- `x` (`int`, default `10`): Left offset.
- `y` (`int`, default `10`): Top offset.
- `opacity` (`float`, default `0.5`): Alpha range `(0, 1]`.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Watermarked MP4 bytes.

### Errors

- Raises `ValueError` for invalid offsets or opacity.


## `overlay_video(base: bytes, pip: bytes, x: int, y: int, width: int | None = None, height: int | None = None, opacity: float = 1.0) -> bytes`

Applies picture-in-picture style overlay onto a base video.

### Detailed Description

`pip` can be image bytes or media bytes. For media, a frame is extracted and overlaid using watermark compositing path.

### Parameters

- `base` (`bytes`): Base video bytes.
- `pip` (`bytes`): Image bytes or media bytes.
- `x` (`int`): Overlay left offset.
- `y` (`int`): Overlay top offset.
- `width` (`int | None`, default `None`): Optional PIP resize width.
- `height` (`int | None`, default `None`): Optional PIP resize height.
- `opacity` (`float`, default `1.0`): Overlay alpha.

### Returns

- `bytes`: MP4 bytes with overlay applied.


## `blur_video(video_data: bytes, sigma: float = 2.0, crf: int = 23, preset: str = "medium") -> bytes`

Applies blur filtering to frames.

### Detailed Description

Blur amount is controlled by `sigma` and internally clamped for stable behavior.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `sigma` (`float`, default `2.0`): Blur strength.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Filtered MP4 bytes.

### Errors

- Raises `ValueError` if `sigma <= 0`.


## `denoise_video(video_data: bytes, strength: float = 0.5, crf: int = 23, preset: str = "medium") -> bytes`

Applies denoise filtering.

### Detailed Description

Intended to reduce visual noise while preserving overall detail.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `strength` (`float`, default `0.5`): Denoise intensity.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Denoised MP4 bytes.

### Errors

- Raises `ValueError` if `strength <= 0`.


## `sharpen_video(video_data: bytes, amount: float = 1.0, crf: int = 23, preset: str = "medium") -> bytes`

Applies sharpening effect to frames.

### Detailed Description

Useful for perceived edge enhancement after blur-heavy or low-detail sources.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `amount` (`float`, default `1.0`): Sharpen amount.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Sharpened MP4 bytes.

### Errors

- Raises `ValueError` if `amount < 0`.


## `color_correct(video_data: bytes, brightness: float = 0.0, contrast: float = 1.0, saturation: float = 1.0, crf: int = 23, preset: str = "medium") -> bytes`

Adjusts brightness, contrast, and saturation.

### Detailed Description

Combines common primary color controls in a single pass for quick grading adjustments.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `brightness` (`float`, default `0.0`): Brightness offset.
- `contrast` (`float`, default `1.0`): Contrast multiplier.
- `saturation` (`float`, default `1.0`): Saturation multiplier.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: Color-adjusted MP4 bytes.


## `apply_lut(video_data: bytes, lut_file_bytes: bytes, crf: int = 23, preset: str = "medium") -> bytes`

Applies LUT-like gamma transform parsed from text data.

### Detailed Description

Current implementation reads LUT text and extracts a `gamma` value for transform.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `lut_file_bytes` (`bytes`): LUT file content.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: LUT-processed MP4 bytes.


## `apply_filtergraph(data: bytes, video_filters: Sequence[str] | str | None = None, audio_filters: Sequence[str] | str | None = None) -> bytes`

Applies supported filter tokens in sequence.

### Detailed Description

Supports current video tokens: `blur=`, `denoise=`, `sharpen=`, `brightness=`, `contrast=`, `saturation=`.
Supports current audio tokens: `volume=`, `normalize`/`normalize=<target>`, `fadein=`, `fadeout=`, `silenceremove`/`silenceremove=<threshold_db>:<min_silence>`.
Applies filters in listed order.

### Parameters

- `data` (`bytes`): Input media bytes.
- `video_filters` (`Sequence[str] | str | None`): Filter token list or comma-separated string.
- `audio_filters` (`Sequence[str] | str | None`): Audio filter token list or comma-separated string.

### Returns

- `bytes`: Filter-processed media bytes.

### Errors

- Raises `ValueError` for unsupported filter tokens.

### Examples

```python
from pymedia import apply_filtergraph

# Video-only tokens
out1 = apply_filtergraph(video_bytes, video_filters=["blur=2.0", "contrast=1.1"])

# Mixed video + audio token pipeline
out2 = apply_filtergraph(
    video_bytes,
    video_filters="denoise=0.7,sharpen=1.1",
    audio_filters=["volume=1.15", "fadeout=0.3"],
)
```


## `stabilize_video(video_data: bytes, strength: int = 16) -> bytes`

Applies lightweight temporal stabilization.

### Detailed Description

Reduces frame-to-frame jitter while keeping audio stream in output.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `strength` (`int`, default `16`): Stabilization strength.

### Returns

- `bytes`: Stabilized MP4 bytes.

### Errors

- Raises `ValueError` if `strength <= 0`.


## `subtitle_burn_in(video_data: bytes, subtitles: str, font_size: int = 24, margin_bottom: int = 24, crf: int = 23, preset: str = "medium") -> bytes`

Burns subtitle text directly into video frames.

### Detailed Description

This produces non-removable subtitles rendered into image frames, unlike soft subtitle tracks.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `subtitles` (`str`): Subtitle content (SRT text).
- `font_size` (`int`, default `24`): Subtitle font size.
- `margin_bottom` (`int`, default `24`): Bottom margin in pixels.
- `crf` (`int`, default `23`): Re-encode quality.
- `preset` (`str`, default `"medium"`): x264 preset.

### Returns

- `bytes`: MP4 bytes with burned subtitles.

### Errors

- Raises `ValueError` for empty subtitle text.
- Raises `ValueError` for invalid font or margin values.


## `create_audio_image_video(audio_data: bytes, images: Sequence[bytes], seconds_per_image: float = 2.0, transition: str = "fade", width: int = 1280, height: int = 720) -> bytes`

Creates slideshow video from audio plus image list.

### Detailed Description

Generates timeline frames from input images, applies transition mode, and muxes with provided audio source.

### Parameters

- `audio_data` (`bytes`): Input containing audio stream.
- `images` (`Sequence[bytes]`): Image payload list.
- `seconds_per_image` (`float`, default `2.0`): Duration per image.
- `transition` (`str`, default `"fade"`): One of `fade`, `slide_left`, `none`.
- `width` (`int`, default `1280`): Output width.
- `height` (`int`, default `720`): Output height.

### Returns

- `bytes`: Slideshow MP4 bytes.

### Errors

- Raises `ValueError` for empty image list, invalid duration/dimensions, unsupported transition, or empty image payload.


## `video_to_gif(video_data: bytes, fps: int = 10, width: int = 320, start: float = 0.0, duration: float = -1.0) -> bytes`

Converts whole video or a segment to animated GIF.

### Detailed Description

Useful for previews, embeds, and short social assets.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `fps` (`int`, default `10`): GIF frame rate.
- `width` (`int`, default `320`): Output width (height auto-derived).
- `start` (`float`, default `0.0`): Segment start.
- `duration` (`float`, default `-1.0`): Segment duration; negative means full media.

### Returns

- `bytes`: GIF file bytes.


## `stack_videos(videos: Sequence[bytes], layout: str = "hstack|vstack|grid") -> bytes`

Composes multiple videos into stacked layout.

### Detailed Description

Current implementation keeps the first input as the dynamic base timeline and
lays out representative frame overlays for the additional inputs according to
the selected layout (`hstack`, `vstack`, or `grid`).

### Parameters

- `videos` (`Sequence[bytes]`): Input clips.
- `layout` (`str`, default `"hstack|vstack|grid"`): Target composition layout.
  Supported values are `hstack`, `horizontal`, `vstack`, `vertical`, and `grid`.
  The default legacy value is treated as `grid`.

### Returns

- `bytes`: Composed MP4 bytes.

### Errors

- Raises `ValueError` for invalid inputs or unsupported layout.


## `split_screen(videos: Sequence[bytes], layout: str = "2x2") -> bytes`

Composes videos into split-screen layout.

### Detailed Description

Builds split-screen layouts by delegating to stacking logic. Supported values:
`2x2`, `2x1`, `1x2`, `hstack`, `vstack`, `grid`.

### Parameters

- `videos` (`Sequence[bytes]`): Input clips.
- `layout` (`str`, default `"2x2"`): Split-screen arrangement string.

### Returns

- `bytes`: Composed MP4 bytes.

### Errors

- Raises `ValueError` for unsupported layout values.
