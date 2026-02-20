# C Modules Layout

`pymedia.c` now keeps shared includes/helpers and pulls feature implementations from these files:

- `audio.c`:
  - audio extraction and advanced audio transcoding
- `video_core.c`:
  - remuxing, frame extraction, re-encode/compress, crop, fps change, padding, flip
- `video_effects.c`:
  - watermark, gif conversion, rotate, speed change, replace audio
- `transforms.c`:
  - volume adjust, merge, reverse, stabilize, subtitle burn-in, slideshow creation
- `metadata.c`:
  - strip/set metadata

This split keeps a single translation unit (via `#include "modules/*.c"`) to avoid linker churn while improving maintainability.
