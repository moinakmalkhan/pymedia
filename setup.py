import os
import shlex
import shutil
import subprocess
import sys

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

# On Windows runners, FFmpeg zip layouts are predictable enough to provide
# a direct fallback when pkg-config is missing or .pc files are absent.
if sys.platform == "win32" and not extra_cflags and not extra_ldflags:
    ffmpeg_root = os.environ.get("FFMPEG_ROOT", "C:/ffmpeg")
    include_dir = os.path.join(ffmpeg_root, "include")
    lib_dir = os.path.join(ffmpeg_root, "lib")
    if os.path.isdir(include_dir):
        extra_cflags.extend([f"-I{include_dir}"])
    if os.path.isdir(lib_dir):
        extra_ldflags.extend(
            [
                f"-L{lib_dir}",
                "-lavformat",
                "-lavcodec",
                "-lavutil",
                "-lswresample",
                "-lswscale",
            ]
        )


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
        if sys.platform == "win32":
            out = os.path.join(out_dir, "libpymedia.dll")
        elif sys.platform == "darwin":
            out = os.path.join(out_dir, "libpymedia.dylib")
        else:
            out = os.path.join(out_dir, "libpymedia.so")

        # Respect the compiler chosen by the build environment (e.g. cibuildwheel sets CC).
        cc = os.environ.get("CC", "gcc")

        if sys.platform == "win32":
            cc_basename = os.path.basename(cc).lower()
            if cc_basename in {"gcc", "clang"} and shutil.which(cc):
                cmd = (
                    [
                        cc,
                        "-shared",
                        "-O2",
                        "-s",
                        "-o",
                        out,
                        src,
                    ]
                    + extra_cflags
                    + extra_ldflags
                )
                print(f"Building libpymedia.dll with {cc}: {' '.join(cmd)}")
                subprocess.check_call(cmd)
            else:
                # Translate pkg-config flags to MSVC-style flags.
                def _msvc_flags(flags):
                    cflags = []
                    ldflags = []
                    for flag in flags:
                        if flag.startswith("-I"):
                            cflags.append("/I" + flag[2:])
                        elif flag.startswith("-D"):
                            cflags.append("/D" + flag[2:])
                        elif flag.startswith("-L"):
                            ldflags.append("/LIBPATH:" + flag[2:])
                        elif flag.startswith("-l"):
                            ldflags.append(flag[2:] + ".lib")
                    return cflags, ldflags

                cflags, ldflags = _msvc_flags(extra_cflags + extra_ldflags)

                # Use MSVC if available (cibuildwheel provides it on windows-latest).
                cmd = (
                    [
                        "cl",
                        "/nologo",
                        "/LD",
                        "/O2",
                        "/MD",
                        src,
                        "/Fe:" + out,
                    ]
                    + cflags
                    + ["/link"]
                    + ldflags
                )

                print(f"Building libpymedia.dll with MSVC: {' '.join(cmd)}")
                subprocess.check_call(cmd)
        else:
            # On macOS cross-compilation cibuildwheel sets ARCHFLAGS="-arch x86_64".
            # We must forward these flags so gcc/clang produces the correct target arch.
            arch_flags = shlex.split(os.environ.get("ARCHFLAGS", ""))

            if sys.platform == "darwin":
                cmd = (
                    [
                        cc,
                        "-shared",
                        "-fPIC",
                        "-O2",
                        "-o",
                        out,
                        src,
                    ]
                    + arch_flags
                    + extra_cflags
                    + extra_ldflags
                )
            else:
                cmd = (
                    [
                        cc,
                        "-shared",
                        "-fPIC",
                        "-O2",
                        "-s",
                        "-o",
                        out,
                        src,
                    ]
                    + arch_flags
                    + extra_cflags
                    + extra_ldflags
                    + ["-lm"]
                )

        print(f"Building libpymedia.so: {' '.join(cmd)}")
        subprocess.check_call(cmd)

        if sys.platform == "win32":
            ffmpeg_root = os.environ.get("FFMPEG_ROOT", "C:/ffmpeg")
            ffmpeg_bin = os.path.join(ffmpeg_root, "bin")
            if os.path.isdir(ffmpeg_bin):
                for name in os.listdir(ffmpeg_bin):
                    if name.lower().endswith(".dll"):
                        src_dll = os.path.join(ffmpeg_bin, name)
                        shutil.copy2(src_dll, out_dir)
            else:
                print(f"Warning: FFmpeg bin directory not found: {ffmpeg_bin}")

        # Also copy in-place for editable installs
        inplace_out = os.path.join(
            HERE,
            "src",
            "pymedia",
            "_lib",
            (
                "libpymedia.dll"
                if sys.platform == "win32"
                else "libpymedia.dylib" if sys.platform == "darwin" else "libpymedia.so"
            ),
        )
        if os.path.abspath(out) != os.path.abspath(inplace_out):
            os.makedirs(os.path.dirname(inplace_out), exist_ok=True)
            shutil.copy2(out, inplace_out)
            if sys.platform == "win32":
                for name in os.listdir(out_dir):
                    if name.lower().endswith(".dll") and name != "libpymedia.dll":
                        shutil.copy2(
                            os.path.join(out_dir, name),
                            os.path.join(os.path.dirname(inplace_out), name),
                        )


class CustomDist(Distribution):
    """Tell setuptools we have ext_modules so build_ext always runs."""

    def has_ext_modules(self):
        return True


setup(
    name="python-media",
    version="0.2.3",
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
    package_data={
        "pymedia": [
            "_lib/libpymedia.so",
            "_lib/libpymedia.dylib",
            "_lib/*.dll",
        ]
    },
    classifiers=[
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: C",
        "Operating System :: POSIX :: Linux",
        "Operating System :: MacOS",
        "Operating System :: Microsoft :: Windows",
        "Topic :: Multimedia :: Video",
        "Topic :: Multimedia :: Sound/Audio",
    ],
    cmdclass={"build_ext": BuildSharedLib},
    distclass=CustomDist,
)
