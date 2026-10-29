# API Reference

## Public classes

### `PrivateAttrBase`

```python
class PrivateAttrBase(metaclass=PrivateAttrType):
    __slots__ = ()
    __private_attrs__ = ()
```

The base class to inherit from to create a class with private attributes. It has
no special behavior on its own; all the machinery lives in its metaclass.

### `PrivateAttrType`

```python
class PrivateAttrType(type):
    def __new__(cls, name, bases, attrs, /, *,
                private_func=None, use_parents=False): ...
```

The metaclass that implements private-attribute rewriting. You rarely use it
directly unless you are building a [custom metaclass](custom-metaclasses.md).

### `PrivateWrapProxy`

```python
class PrivateWrapProxy(Generic[_TVar, _T]):
    def __init__(self, decorator, orig=None, /): ...
    def __call__(self, func, /) -> _PrivateWrap[_T]: ...
```

A decorator that wraps an arbitrary decorator so it remains
private-attribute-aware. See [Decorated methods](guide/decorators.md).

### `_PrivateWrap`

```python
class _PrivateWrap(Generic[_T]):
    @property
    def result(self) -> _T: ...
    @property
    def funcs(self) -> _listproxy[FunctionType]: ...
```

The object returned by `PrivateWrapProxy`. It is transparent: `__getattr__`,
`__call__`, and `__get__` forward to `.result`.

### `_listproxy`

```python
class _listproxy(Generic[_T]):
    def copy(self) -> list[_T]: ...
    def count(self, value, /) -> int: ...
    def index(self, value, start=0, stop=sys.maxsize, /) -> int: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[_T]: ...
    def __getitem__(self, key) -> Any: ...
    def __contains__(self, value) -> bool: ...
```

A read-only proxy over a list. It is returned by `_PrivateWrap.funcs` and
supports the common read-side list interface (`len`, iteration, indexing,
`in`, `copy`, `count`, `index`) but has no mutating methods.

## Functions

### `prepare`

```python
def prepare(name, bases, attrs, /, **kwds) -> _PrivateTemp: ...
```

Context manager for building a private-attribute class inside a custom metaclass.
Returns a temporary object exposing `.name`, `.bases`, `.attrs`, and `.kwds`
(plus, since v2.1.17, a `.postprocess(typ)` shortcut).

### `postprocess`

```python
def postprocess(typ, temp, /) -> None: ...
```

Finalizes a class created via `prepare`. Call it after the metaclass's own
`type.__new__`.

### `register_metaclass`

```python
def register_metaclass(typ, /) -> None: ...
```

Registers a custom metaclass so it can produce private-attribute classes. Must be
called before creating any such class; the metaclass must support weakrefs.

### `ensure_type`

```python
def ensure_type(typ, /) -> None: ...
```

Resets `tp_getattro` / `tp_setattro` / `tp_finalizer` slots on a type.

### `ensure_metaclass`

```python
def ensure_metaclass(typ, /) -> None: ...
```

Resets the corresponding slots on a metaclass.

## Class-definition options

| Option | Type | Default | Meaning |
| --- | --- | --- | --- |
| `__private_attrs__` | `str` \| `Sequence[str]` | `()` | The private attribute names. |
| `private_func` | `(obj_id: int, attr_name: str) -> str` | `None` | Custom hidden-name generator. |
| `use_parents` | `bool` | `False` | Merge ancestors' private names into this class's own set. |
