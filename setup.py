import os
import shutil
import subprocess

from setuptools import find_packages, setup
from setuptools.command.build_ext import build_ext
from setuptools.dist import Distribution

HERE = os.path.dirname(os.path.abspath(__file__))


def pkg_config(packages, flag):
    """Run pkg-config and return the output as a list of strings."""
    try:
        out = (
            subprocess.check_output(
                ["pkg-config", flag] + packages,
                stderr=subprocess.STDOUT,
            )
            .decode()
            .strip()
        )
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
        out_dir = (
            os.path.join(self.build_lib, "pymedia", "_lib")
            if self.build_lib
            else os.path.join(HERE, "src", "pymedia", "_lib")
        )
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, "libpymedia.so")

        cmd = (
            [
                "gcc",
                "-shared",
                "-fPIC",
                "-O2",
                "-s",
                "-o",
                out,
                src,
            ]
            + extra_cflags
            + extra_ldflags
            + ["-lm"]
        )

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
    name="python-media",
    version="0.2.0",
    description="In-memory video processing library for Python, powered by FFmpeg. No temporary files, no subprocesses — everything runs in-process via ctypes.",
    long_description=open(os.path.join(HERE, "README.md"), encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    author="moinakmalkhan",
    url="https://github.com/moinakmalkhan/pymedia",
    project_urls={
        "Bug Tracker": "https://github.com/moinakmalkhan/pymedia/issues",
        "Source Code": "https://github.com/moinakmalkhan/pymedia",
    },
    license="MIT",
    python_requires=">=3.9",
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    package_data={"pymedia": ["_lib/libpymedia.so"]},
    classifiers=[
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: C",
        "Operating System :: POSIX :: Linux",
        "Operating System :: MacOS",
        "Topic :: Multimedia :: Video",
        "Topic :: Multimedia :: Sound/Audio",
    ],
    cmdclass={"build_ext": BuildSharedLib},
    distclass=CustomDist,
)
