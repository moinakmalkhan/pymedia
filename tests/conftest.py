from pathlib import Path

import pytest


@pytest.fixture(scope="session")
def video_data():
    """Load a minimal 1-second MP4 fixture bundled with the tests."""
    sample = Path(__file__).parent / "assets" / "sample.mp4"
    return sample.read_bytes()
