import ctypes
import os
import sys

_LIB_DIR = os.path.join(os.path.dirname(__file__), "_lib")
if sys.platform == "win32":
    _LIB_NAME = "libpymedia.dll"
elif sys.platform == "darwin":
    _LIB_NAME = "libpymedia.dylib"
else:
    _LIB_NAME = "libpymedia.so"
if sys.platform == "win32":
    os.add_dll_directory(_LIB_DIR)
_lib = ctypes.CDLL(os.path.join(_LIB_DIR, _LIB_NAME))

# ── get_video_info ──
_lib.get_video_info.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
]
_lib.get_video_info.restype = ctypes.c_void_p

# ── extract_audio ──
_lib.extract_audio.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.extract_audio.restype = ctypes.POINTER(ctypes.c_uint8)

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
