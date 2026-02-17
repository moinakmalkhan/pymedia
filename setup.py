import subprocess
import os
import shutil
from setuptools import setup
from setuptools.command.build_ext import build_ext
from setuptools.dist import Distribution

HERE = os.path.dirname(os.path.abspath(__file__))


def pkg_config(packages, flag):
    """Run pkg-config and return the output as a list of strings."""
    try:
        out = subprocess.check_output(
            ["pkg-config", flag] + packages,
            stderr=subprocess.STDOUT,
        ).decode().strip()
        return out.split() if out else []
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []


# Query pkg-config for FFmpeg flags
FFMPEG_LIBS = ["libavformat", "libavcodec", "libavutil", "libswresample", "libswscale"]
extra_cflags = pkg_config(FFMPEG_LIBS, "--cflags")
extra_ldflags = pkg_config(FFMPEG_LIBS, "--libs")


class BuildSharedLib(build_ext):
    """Custom build step that compiles pymedia.c into a shared library."""

    def run(self):
        src = os.path.join(HERE, "src", "pymedia", "_lib", "pymedia.c")

        # Build into build_lib for wheel
        out_dir = os.path.join(self.build_lib, "pymedia", "_lib") if self.build_lib else \
                  os.path.join(HERE, "src", "pymedia", "_lib")
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, "libpymedia.so")

        cmd = [
            "gcc", "-shared", "-fPIC", "-o", out, src,
        ] + extra_cflags + extra_ldflags + ["-lm"]

        print(f"Building libpymedia.so: {' '.join(cmd)}")
        subprocess.check_call(cmd)

        # Also copy in-place for editable installs
        inplace_out = os.path.join(HERE, "src", "pymedia", "_lib", "libpymedia.so")
        if os.path.abspath(out) != os.path.abspath(inplace_out):
            os.makedirs(os.path.dirname(inplace_out), exist_ok=True)
            shutil.copy2(out, inplace_out)


class CustomDist(Distribution):
    """Tell setuptools we have ext_modules so build_ext always runs."""
    def has_ext_modules(self):
        return True


setup(
    cmdclass={"build_ext": BuildSharedLib},
    distclass=CustomDist,
    package_data={"pymedia": ["_lib/libpymedia.so"]},
)
