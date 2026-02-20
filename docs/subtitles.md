# Subtitles API

## `convert_subtitles(sub_data: bytes | str, src: str = "srt", dst: str = "vtt") -> str`

Converts subtitle text between supported textual formats.

### Detailed Description

This function converts subtitle payloads in memory without writing files. Current conversion path support is:

- `srt -> vtt`
- `vtt -> srt`
- `ass <-> ssa` passthrough (text returned unchanged)

If `src == dst`, content is returned as-is (bytes input is decoded to UTF-8 with replacement).

### Parameters

- `sub_data` (`bytes | str`): Subtitle content.
- `src` (`str`, default `"srt"`): Source format.
- `dst` (`str`, default `"vtt"`): Target format.

### Returns

- `str`: Converted subtitle text.

### Errors

- Raises `ValueError` for unsupported conversion pairs.


## `extract_subtitles(video_data: bytes) -> list[dict]`

Extracts soft subtitle track metadata from media.

### Detailed Description

This function inspects subtitle streams in the container and returns structured entries with stream-level details (for example codec, language, and preview text fields when available).

### Parameters

- `video_data` (`bytes`): Input media bytes.

### Returns

- `list[dict]`: Subtitle stream metadata list.

### Errors

- Raises `RuntimeError` if extraction fails in native layer.


## `add_subtitle_track(video_data: bytes, subtitles: str | bytes, lang: str = "eng", codec: str = "mov_text") -> bytes`

Adds a soft subtitle stream to media.

### Detailed Description

This function muxes subtitle text into the output container as a subtitle track. It does not burn text into video frames. Use this when you want selectable subtitle streams.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `subtitles` (`str | bytes`): Subtitle text payload (typically SRT).
- `lang` (`str`, default `"eng"`): Language tag for subtitle stream.
- `codec` (`str`, default `"mov_text"`): Subtitle codec. Supported: `mov_text`, `subrip`, `srt`.

### Returns

- `bytes`: Media bytes containing the newly added subtitle track.

### Errors

- Raises `ValueError` if subtitle text is empty.
- Raises `ValueError` if codec is unsupported.


## `remove_subtitle_tracks(video_data: bytes, language: str | None = None) -> bytes`

Removes subtitle streams from media.

### Detailed Description

This function outputs media with subtitle tracks removed. The optional `language` argument is accepted for API compatibility; current native implementation removes subtitle streams without language-specific filtering.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `language` (`str | None`, default `None`): Optional language selector.

### Returns

- `bytes`: Media bytes without subtitle tracks.
