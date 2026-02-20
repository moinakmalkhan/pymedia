# Installation

## End User Installation

Install from PyPI:

```bash
python -m pip install python-media
```

## Development Installation

### 1. Create virtual environment

```bash
python -m venv .venv
source .venv/bin/activate
```

### 2. Install build/runtime tooling

```bash
python -m pip install -U pip setuptools wheel
pip install -e .
pip install pytest
```

### 3. Build native extension (if needed)

```bash
python setup.py build_ext --inplace
```

### 4. Verify install

```bash
python -c "import pymedia; print('ok')"
```

### 5. Run tests

```bash
pytest tests/ -v
```

## Notes

- The package loads a platform-specific shared library (`libpymedia.so` / `.dylib` / `.dll`).
- In source checkouts, import-time load may attempt an in-place build if the library is missing.
- Local source builds require FFmpeg development headers/libraries on your system.
