# Metadata API

## `set_metadata(video_data: bytes, key: str, value: str) -> bytes`

Adds or updates a metadata tag in media.

### Detailed Description

This function writes one metadata key-value pair (for example `title`, `artist`, `comment`, `year`) while preserving existing metadata fields whenever possible.

### Parameters

- `video_data` (`bytes`): Input media bytes.
- `key` (`str`): Metadata field name.
- `value` (`str`): Metadata field value.

### Returns

- `bytes`: Media bytes with updated metadata.

### Errors

- Raises `ValueError` if `key` is empty.


## `strip_metadata(video_data: bytes) -> bytes`

Removes metadata tags from media output.

### Detailed Description

This function outputs a version of the input media with metadata fields removed. It is useful for privacy cleanup and deterministic content pipelines.

### Parameters

- `video_data` (`bytes`): Input media bytes.

### Returns

- `bytes`: Media bytes without metadata tags.
