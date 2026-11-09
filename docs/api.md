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

### `PrivateSuper`

```python
class PrivateSuper:
    def __init__(self, A, B, C, obj, /): ...
```

Explicit cross-class access to a parent class's class-level private attribute.

- `A`: the type whose code is currently running (the code owner).
- `B`: a parent type of `A` whose class-level private attribute is accessed.
- `C`: the class the private variable was defined for; it must be `B`, `A`, or
  a class that is both a subclass of `B` and a parent of `A`.
- `obj`: an instance of `A`.

`A`, `B` and `C` must all be private-attribute types (instances of
`PrivateAttrType` or a registered metaclass). At attribute access, the running
code must belong to `A`; the attribute is then resolved from `B`'s class-level
storage for `C` (i.e. `all_type_subclass_attr[B][C]`, or `B`'s own body storage
when `C is B`) and its `__get__(obj, A)` is invoked.

> This is **not** for instance-attribute cross-type access.

### `PrivateClassSuper`

```python
class PrivateClassSuper:
    def __init__(self, A, B, C, cls, /): ...
```

The classmethod flavour of `PrivateSuper`. `A`, `B` and `C` are as above;
`cls` is a subclass of `A`. The attribute is resolved from the same storage and
its `__get__(None, cls)` is invoked.

## Functions

### `create_new`

```python
def create_new(func=None, /) -> Callable[..., Any]: ...
```

Registers and controls a custom metaclass automatically. Place it in the
metaclass body (or use it as a decorator) and it replaces the old
`prepare` / `postprocess` / `register_metaclass` flow:

```python
from abc import ABCMeta
import private_attribute

class PrivateAbcMeta(ABCMeta):
    __new__ = private_attribute.create_new()

# or as a decorator:
class PrivateAbcMeta2(ABCMeta):
    @private_attribute.create_new
    def __new__(cls, name, bases, attrs, **kwargs):
        return super().__new__(cls, name, bases, attrs, **kwargs)
```

With no argument the default `super().__new__` is used; with a callback, the
callback is invoked in place of `type.__new__` and the resulting type is
finalized as a private-attribute class.

### `create_meta_slot`

```python
def create_meta_slot(name: str, /): ...
```

Creates the final slot for a registered metaclass. The slot name must be one of
`__getattribute__`, `__getattr__`, `__setattr__`, `__delattr__`, `__del__`, or
`__call__`. Pass the name first, then either call the result with no arguments
for an empty slot, or use it as a decorator with a callback:

```python
class PrivateAbcMeta(ABCMeta):
    __new__ = private_attribute.create_new()
    __setattr__ = private_attribute.create_meta_slot("__setattr__")()

    @private_attribute.create_meta_slot("__getattr__")
    def __getattr__(cls, name): ...
```

### `create_slot`

```python
def create_slot(name: str, /): ...
```

Creates the final slot for a private-attribute class. The slot name must be one
of `__getattribute__`, `__getattr__`, `__setattr__`, `__delattr__`, or
`__del__`. Pass the name first, then either call the result with no arguments
for an empty slot, or use it as a decorator with a callback:

```python
class MyClass(private_attribute.PrivateAttrBase):
    __setattr__ = private_attribute.create_slot("__setattr__")()

    @private_attribute.create_slot("__delattr__")
    def __delattr__(self, name): ...
```

### `ensure_type`

```python
def ensure_type(typ, /) -> None: ...
```

Re-injects the special-method slot descriptors (`__getattribute__` /
`__getattr__` / `__setattr__` / `__delattr__` / `__del__`) and the
`tp_traverse` / `tp_clear` wrappers after CPython has reset them.

### `ensure_metaclass`

```python
def ensure_metaclass(typ, /) -> None: ...
```

Re-injects the metaclass slot descriptors (plus `__call__`) and the
`tp_traverse` / `tp_clear` wrappers.

### Deprecated: `prepare` / `postprocess` / `register_metaclass`

These three functions are deprecated and emit a warning when accessed. They are
kept for backward compatibility; new code should use `create_new` (and, for
custom slots, `create_meta_slot` / `create_slot`) instead.

## Class-definition options

| Option | Type | Default | Meaning |
| --- | --- | --- | --- |
| `__private_attrs__` | `str` \| `Sequence[str]` | `()` | The private attribute names. |
| `private_func` | `(obj_id: int, attr_name: str) -> str` | `None` | Custom hidden-name generator. |
| `use_parents` | `bool` | `False` | Merge ancestors' private names into this class's own set. |
