from __future__ import annotations

import ctypes
import json
import re

from pymedia._core import _call_bytes_fn, _lib


def _normalize_newlines(text: str) -> str:
    """Normalize all newline variants to `\\n`."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _srt_to_vtt(srt_text: str) -> str:
    """Convert SRT subtitle text into WebVTT text."""
    s = _normalize_newlines(srt_text).strip()
    if not s:
        return "WEBVTT\n\n"

    lines = s.split("\n")
    out = ["WEBVTT", ""]
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if not line:
            out.append("")
            i += 1
            continue

        if re.fullmatch(r"\d+", line):
            i += 1
            if i >= len(lines):
                break
            line = lines[i].strip()

        if "-->" in line:
            out.append(line.replace(",", "."))
            i += 1
            while i < len(lines) and lines[i].strip():
                out.append(lines[i])
                i += 1
            out.append("")
        else:
            i += 1

    return "\n".join(out).rstrip() + "\n"


def _vtt_to_srt(vtt_text: str) -> str:
    """Convert WebVTT subtitle text into SRT text."""
    s = _normalize_newlines(vtt_text).strip()
    if s.startswith("WEBVTT"):
        s = s[len("WEBVTT") :].lstrip("\n")

    blocks = [b for b in s.split("\n\n") if b.strip()]
    out = []
    idx = 1
    for block in blocks:
        lines = block.split("\n")
        if not lines:
            continue
        time_line = lines[0]
        if "-->" not in time_line and len(lines) > 1 and "-->" in lines[1]:
            lines = lines[1:]
            time_line = lines[0]
        if "-->" not in time_line:
            continue

        out.append(str(idx))
        out.append(time_line.replace(".", ","))
        out.extend(lines[1:])
        out.append("")
        idx += 1

    return "\n".join(out).rstrip() + "\n"


def convert_subtitles(sub_data: bytes | str, src: str = "srt", dst: str = "vtt") -> str:
    """Convert subtitles between supported text formats.

    Args:
        sub_data: Subtitle content as `bytes` or `str`.
        src: Source format (`srt`, `vtt`, `ass`, `ssa`).
        dst: Target format.

    Returns:
        Converted subtitle text.
    """
    src = src.lower().strip()
    dst = dst.lower().strip()
    if src == dst:
        if isinstance(sub_data, bytes):
            return sub_data.decode("utf-8", errors="replace")
        return sub_data

    text = sub_data.decode("utf-8", errors="replace") if isinstance(sub_data, bytes) else sub_data

    if src == "srt" and dst == "vtt":
        return _srt_to_vtt(text)
    if src == "vtt" and dst == "srt":
        return _vtt_to_srt(text)
    if src in {"ass", "ssa"} and dst in {"ass", "ssa"}:
        return text
    raise ValueError("Unsupported subtitle conversion; supported: srt<->vtt")


def extract_subtitles(video_data: bytes) -> list[dict]:
    """Extract soft subtitle tracks as structured metadata.

    Args:
        video_data: In-memory media bytes.

    Returns:
        List of subtitle stream dictionaries with codec/language/text preview.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    result_ptr = _lib.extract_subtitles_json(buf, len(video_data))
    if not result_ptr:
        raise RuntimeError("Failed to extract subtitles")
    try:
        return json.loads(ctypes.string_at(result_ptr).decode("utf-8"))
    finally:
        _lib.pymedia_free(result_ptr)


def add_subtitle_track(
    video_data: bytes, subtitles: str | bytes, lang: str = "eng", codec: str = "mov_text"
) -> bytes:
    """Mux subtitles as a soft track into media.

    Args:
        video_data: In-memory media bytes.
        subtitles: Subtitle text (typically SRT).
        lang: ISO language tag.
        codec: Target subtitle codec (`mov_text`, `subrip`, `srt`).

    Returns:
        Media bytes including the added subtitle stream.
    """
    if isinstance(subtitles, bytes):
        subtitles = subtitles.decode("utf-8", errors="replace")
    if not subtitles.strip():
        raise ValueError("subtitles must be non-empty")
    if codec not in {"mov_text", "subrip", "srt"}:
        raise ValueError("codec must be one of: mov_text, subrip, srt")
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    return _call_bytes_fn(
        _lib.add_subtitle_track,
        buf,
        len(video_data),
        subtitles.encode("utf-8"),
        lang.encode("utf-8"),
        codec.encode("utf-8"),
    )


def remove_subtitle_tracks(video_data: bytes, language: str | None = None) -> bytes:
    """Remove subtitle streams from media.

    Args:
        video_data: In-memory media bytes.
        language: Optional language filter (currently ignored in native path).

    Returns:
        Media bytes with subtitle streams removed.
    """
    buf = (ctypes.c_uint8 * len(video_data)).from_buffer_copy(video_data)
    lang = None if language is None else language.encode("utf-8")
    return _call_bytes_fn(_lib.remove_subtitle_tracks, buf, len(video_data), lang)
