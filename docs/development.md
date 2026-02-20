# Development Guide

## Repository Layout

- `src/pymedia/`: Python public API wrappers.
- `src/pymedia/_core.py`: Native library loading and ctypes signatures.
- `src/pymedia/_lib/modules/`: Native C implementation split by domain.
- `tests/`: Unit/integration tests using in-memory media fixtures.

## Local Workflow

### 1. Setup

```bash
python -m venv .venv
source .venv/bin/activate
python -m pip install -U pip setuptools wheel
pip install -e .
pip install pytest
```

### 2. Build native layer

```bash
python setup.py build_ext --inplace
```

### 3. Run tests

```bash
pytest tests/ -v
```

## Development Rules

- Keep Python wrappers thin and focused on validation + argument marshalling.
- Implement heavy media logic in native modules under `src/pymedia/_lib/modules/`.
- Add tests for every public API addition and validation branch.
- Keep docs synchronized with actual function signatures and behavior.
