# Features

## Video

Implemented:

- Container remuxing (`convert_format`)
- Video transcoding (H.264) with bitrate/CRF controls (`transcode_video`)
- Trimming/cutting/splitting (`trim_video`, `cut_video`, `split_video`)
- Geometry and timing transforms (`resize_video`, `crop_video`, `pad_video`, `flip_video`, `rotate_video`, `change_fps`, `change_speed`)
- Stream composition (`merge_videos`, `concat_videos`, `replace_audio`, `change_video_audio`)
- Visual effects (`blur_video`, `denoise_video`, `sharpen_video`, `color_correct`, `apply_lut`, `apply_filtergraph`, `add_watermark`, `overlay_video`, `stabilize_video`)
- Subtitle burn-in (`subtitle_burn_in`)
- Slideshow generation (`create_audio_image_video`)
- GIF export (`video_to_gif`)

Pending:

- Advanced stream-synchronized multi-input composition refinements

## Audio

Implemented:

- Audio extraction/transcoding (`extract_audio`, `transcode_audio`)
- Volume and dynamics helpers (`adjust_volume`, `fade_audio`, `normalize_audio_lufs`)
- Resampling/bitrate conversion (`resample_audio`, `change_audio_bitrate`)
- Silence analysis/editing (`silence_detect`, `silence_remove`)
- Multi-track operations (`crossfade_audio`, `mix_audio_tracks`)

## Frames and Metadata

Implemented:

- Single/multi-frame extraction (`extract_frame`, `extract_frames`)
- Thumbnails and timeline previews (`create_thumbnail`, `generate_preview`)
- Metadata set/remove (`set_metadata`, `strip_metadata`)
- Media probing (`get_video_info`)

## Analysis

Implemented:

- Keyframe listing (`list_keyframes`)
- Scene-change approximation (`detect_scenes`)
- Keyframe-safe trim (`trim_to_keyframes`)
- Re-encoded boundary trim (`frame_accurate_trim`)

## Subtitles

Implemented:

- Text conversion (`convert_subtitles`, `srt <-> vtt`)
- Subtitle track extraction (`extract_subtitles`)
- Add/remove subtitle tracks (`add_subtitle_track`, `remove_subtitle_tracks`)

## Streaming / Packaging

Implemented:

- Fragmented MP4 generation (`create_fragmented_mp4`)
- Stream-copy remux path (`stream_copy`)
- Probe and analysis utilities (`probe_media`, `analyze_loudness`, `analyze_gop`, `detect_vfr_cfr`)

Pending:

- Advanced HLS/DASH output options (encryption, richer manifest profiles)
