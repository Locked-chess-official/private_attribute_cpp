# Installation

## From PyPI

```bash
pip install private-attribute-cpp
```

Prebuilt wheels are published for CPython **3.10–3.15** — including the
free-threaded (`t`, no-GIL) builds introduced in 3.13 — across **Linux**,
**macOS**, and **Windows**.

## Android

Android wheels for **Termux** and **Pydroid3** are not published to PyPI (their
platform tags would collide). Instead they are attached to the matching GitHub
releases:

| Release tag | Builds |
| --- | --- |
| `vX.Y.Z` | PyPI wheels (via `python-publish.yml`) |
| `vX.Y.Z-termux` | Termux wheels only |
| `vX.Y.Z-android` | Pydroid3 wheels only |

## From source

The module is built with
[scikit-build-core](https://scikit-build-core.readthedocs.io/) and CMake. It
vendors the header-only [PicoSHA2](https://github.com/okdshin/PicoSHA2) library
for SHA-256 hashing, so there are no external C++ dependencies.

```bash
git clone https://github.com/Locked-chess-official/private_attribute_cpp.git
cd private_attribute_cpp
pip install .
```

To build a wheel instead:

```bash
pip install build
python -m build
```

!!! tip "Debug symbols"

    Set `PRIVATE_ATTR_KEEP_DEBUG=1` to retain debug symbols in a Linux build
    (otherwise `-g0` is applied by CMake). The CI test workflow sets this flag
    so native crash backtraces carry full symbols.

## Requirements

- A **CPython** interpreter (PyPy and other implementations are not supported).
- Python ≥ 3.10.
- A C++17 compiler to build from source.
