import subprocess
import tempfile
import os
import pytest


@pytest.fixture(scope="session")
def video_data():
    """Generate a minimal 1-second test video with audio using ffmpeg."""
    with tempfile.NamedTemporaryFile(suffix=".mp4", delete=False) as f:
        path = f.name

    try:
        subprocess.run([
            "ffmpeg", "-y",
            "-f", "lavfi", "-i", "color=black:size=64x64:rate=10:duration=1",
            "-f", "lavfi", "-i", "sine=frequency=440:duration=1",
            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "51",
            "-c:a", "aac", "-b:a", "32k",
            "-shortest", path,
        ], capture_output=True, check=True)

        with open(path, "rb") as f:
            return f.read()
    finally:
        os.unlink(path)
