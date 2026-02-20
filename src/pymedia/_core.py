import ctypes
import os
import subprocess
import sys
from pathlib import Path

_LIB_DIR = os.path.join(os.path.dirname(__file__), "_lib")
if sys.platform == "win32":
    _LIB_NAME = "libpymedia.dll"
elif sys.platform == "darwin":
    _LIB_NAME = "libpymedia.dylib"
else:
    _LIB_NAME = "libpymedia.so"


def _load_native_lib():
    """Load the platform-native `libpymedia` shared library.

    Behavior:
    - Load bundled library directly when present.
    - In source checkouts, attempt one in-place native build so imports
      can succeed during local development/test runs.
    - Raise a descriptive `OSError` with build output if loading fails.
    """
    base_dir = Path(__file__).resolve().parent
    lib_path = base_dir / "_lib" / _LIB_NAME

    if sys.platform == "win32":
        os.add_dll_directory(str(lib_path.parent))

    if lib_path.exists():
        return ctypes.CDLL(str(lib_path))

    # In source checkouts, tests may run before an editable install.
    # Try building in-place once so import-time collection can proceed.
    project_root = base_dir.parent.parent
    setup_py = project_root / "setup.py"
    build_error = None
    if setup_py.exists():
        try:
            completed = subprocess.run(
                [sys.executable, "setup.py", "build_ext", "--inplace"],
                cwd=str(project_root),
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if completed.returncode != 0:
                build_error = (completed.stdout or "") + "\n" + (completed.stderr or "")
        except subprocess.CalledProcessError as exc:
            build_error = (exc.stdout or "") + "\n" + (exc.stderr or "")
            if not build_error.strip():
                build_error = str(exc)
        except Exception as exc:
            build_error = str(exc)

    if lib_path.exists():
        return ctypes.CDLL(str(lib_path))

    message = (
        f"{lib_path} is missing. Run `pip install -e .` (or `python setup.py build_ext --inplace`) "
        "to build the native library before importing pymedia."
    )
    if build_error:
        message += f"\nBuild attempt output:\n{build_error}"
    raise OSError(message)


_lib = _load_native_lib()

# ── get_video_info ──
_lib.get_video_info.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
]
_lib.get_video_info.restype = ctypes.c_void_p

# ── list_keyframes_json ──
_lib.list_keyframes_json.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
]
_lib.list_keyframes_json.restype = ctypes.c_void_p

# ── list_video_packet_timestamps_json ──
_lib.list_video_packet_timestamps_json.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
]
_lib.list_video_packet_timestamps_json.restype = ctypes.c_void_p

# ── extract_subtitles_json ──
_lib.extract_subtitles_json.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
]
_lib.extract_subtitles_json.restype = ctypes.c_void_p

# ── extract_audio ──
_lib.extract_audio.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.extract_audio.restype = ctypes.POINTER(ctypes.c_uint8)

# ── transcode_audio_advanced ──
_lib.transcode_audio_advanced.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.transcode_audio_advanced.restype = ctypes.POINTER(ctypes.c_uint8)

# ── convert_format ──
_lib.convert_format.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.convert_format.restype = ctypes.POINTER(ctypes.c_uint8)

# ── trim_video ──
_lib.trim_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_double,
    ctypes.c_double,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.trim_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── mute_video ──
_lib.mute_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.mute_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── extract_frame ──
_lib.extract_frame.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_double,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.extract_frame.restype = ctypes.POINTER(ctypes.c_uint8)

# ── reencode_video ──
_lib.reencode_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.reencode_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── transcode_video_bitrate ──
_lib.transcode_video_bitrate.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.transcode_video_bitrate.restype = ctypes.POINTER(ctypes.c_uint8)

# ── crop_video ──
_lib.crop_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.crop_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── change_fps ──
_lib.change_fps.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_double,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.change_fps.restype = ctypes.POINTER(ctypes.c_uint8)

# ── pad_video ──
_lib.pad_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.pad_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── flip_video ──
_lib.flip_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.flip_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── create_fragmented_mp4 ──
_lib.create_fragmented_mp4.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.create_fragmented_mp4.restype = ctypes.POINTER(ctypes.c_uint8)

# ── filter_video_basic ──
_lib.filter_video_basic.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.c_double,
    ctypes.c_double,
    ctypes.c_double,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.filter_video_basic.restype = ctypes.POINTER(ctypes.c_uint8)

# ── add_watermark ──
_lib.add_watermark.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_double,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.add_watermark.restype = ctypes.POINTER(ctypes.c_uint8)

# ── video_to_gif ──
_lib.video_to_gif.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_double,
    ctypes.c_double,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.video_to_gif.restype = ctypes.POINTER(ctypes.c_uint8)

# ── rotate_video ──
_lib.rotate_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.rotate_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── change_speed ──
_lib.change_speed.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_double,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.change_speed.restype = ctypes.POINTER(ctypes.c_uint8)

# ── stabilize_video ──
_lib.stabilize_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.stabilize_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── subtitle_burn_in ──
_lib.subtitle_burn_in.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.subtitle_burn_in.restype = ctypes.POINTER(ctypes.c_uint8)

# ── create_audio_image_video ──
_lib.create_audio_image_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_int,
    ctypes.c_double,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.create_audio_image_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── adjust_volume ──
_lib.adjust_volume.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_double,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.adjust_volume.restype = ctypes.POINTER(ctypes.c_uint8)

# ── merge_videos ──
_lib.merge_videos.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.merge_videos.restype = ctypes.POINTER(ctypes.c_uint8)

# ── reverse_video ──
_lib.reverse_video.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.reverse_video.restype = ctypes.POINTER(ctypes.c_uint8)

# ── strip_metadata ──
_lib.strip_metadata.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.strip_metadata.restype = ctypes.POINTER(ctypes.c_uint8)

# ── set_metadata ──
_lib.set_metadata.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.set_metadata.restype = ctypes.POINTER(ctypes.c_uint8)

# ── replace_audio ──
_lib.replace_audio.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.replace_audio.restype = ctypes.POINTER(ctypes.c_uint8)

# ── add_subtitle_track ──
_lib.add_subtitle_track.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.add_subtitle_track.restype = ctypes.POINTER(ctypes.c_uint8)

# ── remove_subtitle_tracks ──
_lib.remove_subtitle_tracks.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.remove_subtitle_tracks.restype = ctypes.POINTER(ctypes.c_uint8)

# ── allocator bridge ──
_lib.pymedia_free.argtypes = [ctypes.c_void_p]
_lib.pymedia_free.restype = None


def _call_bytes_fn(fn, *args):
    """Call a C function that returns uint8_t* + out_size, return bytes."""
    out_size = ctypes.c_size_t()
    result_ptr = fn(*args, ctypes.byref(out_size))
    if not result_ptr:
        raise RuntimeError("Operation failed")
    data = ctypes.string_at(result_ptr, out_size.value)
    _lib.pymedia_free(result_ptr)
    return data
