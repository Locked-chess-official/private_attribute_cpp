# Contributing

## Getting the code

```bash
git clone https://github.com/Locked-chess-official/private_attribute_cpp.git
cd private_attribute_cpp
pip install -e .
```

## Running the tests

```bash
pip install pytest
python -m pytest tests/ -v
```

The suite covers:

- attribute hiding (instance, class, and method)
- custom `private_func`
- decorator wrapping (`PrivateWrapProxy`)
- inheritance semantics (`use_parents`, parent/child storage separation)
- ABC / metaclass integration
- `__slots__` and cyclic-GC collection regressions

## Linting

The repository uses [pre-commit](https://pre-commit.com):

```bash
pip install pre-commit
pre-commit run --all-files
```

## Building

```bash
pip install build
python -m build
```

See [Installation](installation.md) for the full build matrix and the
`PRIVATE_ATTR_KEEP_DEBUG` flag.

## CI

| Workflow | Purpose |
| --- | --- |
| `test.yml` | Build + test across Linux/macOS/Windows × CPython 3.10–3.15 (incl. `t` builds). |
| `python-publish.yml` | Build and upload PyPI wheels on plain `vX.Y.Z` releases. |
| `lint.yml` | Run pre-commit hooks. |
| `update-bot.yml` | Monthly automated update of vendored PicoSHA2. |

## Docs

The documentation site is built with [MkDocs Material](https://squidfunk.github.io/mkdocs-material/):

```bash
pip install mkdocs-material
mkdocs serve      # local preview at http://127.0.0.1:8000
mkdocs build      # static build into site/
```
