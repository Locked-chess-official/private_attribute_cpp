# private_attribute_cpp

[![PyPI](https://img.shields.io/pypi/v/private-attribute-cpp)](https://pypi.org/project/private-attribute-cpp/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

True C++-style **private attributes** for Python, enforced at the C level.

A C++ extension module (`private_attribute`) that lets you declare attributes
that are only accessible from *inside* the class that owns them — genuinely
hidden from the outside world, from subclasses, and from `hasattr()`, `dir()`,
and `obj.__dict__`. Unlike Python's name-mangling (`_Class__name`) and the
leading-underscore convention, these attributes cannot be reached at all from
outside the declaring class's own code.

> **Note:** CPython only. Requires Python ≥ 3.10.

---

## Table of contents

- [Why?](#why)
- [Installation](#installation)
- [Quick start](#quick-start)
- [Core concepts](#core-concepts)
  - [Declaring private attributes](#declaring-private-attributes)
  - [Custom name generation (`private_func`)](#custom-name-generation-private_func)
  - [Decorated methods (`PrivateWrapProxy`)](#decorated-methods-privatewrapproxy)
  - [Inheritance and `use_parents`](#inheritance-and-use_parents)
- [API reference](#api-reference)
- [Custom metaclasses](#custom-metaclasses)
- [Behavioral notes & limitations](#behavioral-notes--limitations)
- [Building from source](#building-from-source)
- [Running the tests](#running-the-tests)
- [License](#license)

---

## Why?

Python gives you a *convention* (`_private`) and a *name-mangling trick*
(`__name` → `_ClassName__name`), but neither is an actual access barrier. Any
code that knows (or can guess) the mangled name can still read and mutate your
"private" state.

`private_attribute` implements the real thing in C++:

- Private attributes are **not stored in `__dict__`**.
- They are **unreachable from outside** the class that declares them — including
  from subclasses and from `getattr`/`hasattr`/`dir`.
- Inside the declaring class, they look and behave like ordinary attributes.

It does this by rewriting attribute access in `tp_getattro` / `tp_setattro` and
storing private values under generated, collision-resistant names (a tuple of
two SHA-256 hashes).

---

## Installation

```bash
pip install private-attribute-cpp
```

Prebuilt wheels are published for CPython 3.10–3.15 (including free-threaded
`t` builds) across Linux, macOS, and Windows.

---

## Quick start

```python
from private_attribute import PrivateAttrBase

class BankAccount(PrivateAttrBase):
    __private_attrs__ = ["_balance"]

    def __init__(self, balance):
        self._balance = balance

    def deposit(self, amount):
        self._balance += amount      # ✅ accessible inside the class

    @property
    def balance(self):
        return self._balance

acct = BankAccount(100)
print(acct.balance)     # 100
acct.deposit(50)
print(acct.balance)     # 150

print(hasattr(acct, "_balance"))   # False — truly hidden
print(acct._balance)               # AttributeError: private attribute
```

---

## Core concepts

### Declaring private attributes

List the private names in the `__private_attrs__` class attribute:

```python
from private_attribute import PrivateAttrBase

class MyClass(PrivateAttrBase):
    __private_attrs__ = ["a", "b", "c"]   # string, or a sequence of strings

    def __init__(self):
        self.a = 1
        self.b = 2
        self.c = 3

    def show(self):
        print(self.a, self.b, self.c)     # (1, 2, 3) — visible inside

obj = MyClass()
obj.show()                                # 1 2 3
print(hasattr(obj, "a"))                  # False
print(hasattr(obj, "b"))                  # False
print(hasattr(obj, "c"))                  # False
```

- Private attributes may be set on **instances** *or* on the **class itself**
  (class-level private state, e.g. for `@classmethod`).
- They also cover **private methods**: declare a method name in
  `__private_attrs__` and it becomes callable only from the class's own code.
- They work alongside `__slots__` (see notes below).

### Custom name generation (`private_func`)

By default private values are stored under generated hash names. You can
control how the storage name is derived with a `private_func` callable, which
receives the object id and the attribute name:

```python
from private_attribute import PrivateAttrBase

def my_generate_func(obj_id, attr_name):
    return f"_hidden_{obj_id}_{attr_name}"

class MyClass(PrivateAttrBase, private_func=my_generate_func):
    __private_attrs__ = ["secret"]

    def __init__(self):
        self.secret = 42

    def read(self):
        return self.secret
```

`private_func` may be passed either as a keyword in the class definition (shown
above) or on `PrivateAttrBase` itself. It is optional; the default generates a
tuple of two SHA-256 hashes.

### Decorated methods (`PrivateWrapProxy`)

Decorators that return a plain wrapper object (anything other than
`property` / `classmethod` / `staticmethod`) would normally break private
attribute access, because the wrapper's bytecode is not recognized as
belonging to the class. `PrivateWrapProxy` makes arbitrary decorators
compatible:

```python
from private_attribute import PrivateAttrBase, PrivateWrapProxy

def deco(func):
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs)
    return wrapper

class MyClass(PrivateAttrBase):
    __private_attrs__ = ["a", "b"]

    @PrivateWrapProxy(deco)
    def method1(self):
        return self.a

    @PrivateWrapProxy(deco)
    @PrivateWrapProxy(deco)
    def method2(self):
        return self.b
```

`PrivateWrapProxy(decorator)` returns a `_PrivateWrap` object that wraps the
decorator application. It exposes two public attributes:

- **`.result`** — the final decorated result.
- **`.funcs`** — the tuple of original (pre-decoration) functions.

To stack decorators, resolve name conflicts, or fix decorator ordering, chain
`.result.<attr>` onto the wrapped method and pass the method itself as the
second argument (which saves the previous functions):

```python
class MyClass(PrivateAttrBase):
    __private_attrs__ = ["a", "b", "c"]

    @PrivateWrapProxy(deco1)
    def method1(self):
        return self.a

    @PrivateWrapProxy(method1.result.name1, method1)
    @PrivateWrapProxy(method1.result.name2, method1)
    def method1(self):        # final implementation; earlier ones are kept
        return self.b

    @PrivateWrapProxy(deco2)
    def method2(self):
        return self.c
```

The `_PrivateWrap` object is transparent in normal use: it forwards
`__getattr__`, `__call__`, and descriptor `__get__` to its `.result`, so callers
interact with the wrapped method as usual.

### Inheritance and `use_parents`

Since v2.1.0, **a subclass's code cannot access its parent's private
attributes** — a parent's private attribute is reachable only from the parent's
own code, or from a class that re-declares the same name in its own
`__private_attrs__`. When a subclass re-declares the same name, the two are
stored **separately** (instance attributes per declaring class, class-level
attributes per class).

Since v2.2.0, you can opt in to inheriting all ancestors' private names with
`use_parents=True`:

```python
class Parent(PrivateAttrBase):
    __private_attrs__ = ["_secret"]
    def __init__(self):
        self._secret = 1
    def get_secret(self):
        return self._secret

class Child(Parent, use_parents=True):
    __private_attrs__ = []
    def __init__(self):
        super().__init__()
        self._secret = 99        # stored in Child's own storage
    def read_own(self):
        return self._secret

obj = Child()
obj.get_secret()   # 1  — parent code sees the parent's value
obj.read_own()     # 99 — child code sees its own value
```

The merged names live in the subclass's **own** storage — they never overwrite
the parent's values. The Python-level `__private_attrs__` tuple is left
unchanged; only the internal name set is merged. Default is `False`.

---

## API reference

| Name | Kind | Purpose |
| --- | --- | --- |
| `PrivateAttrBase` | class (metaclass `PrivateAttrType`) | Inherit from this to make a class with private attributes. |
| `PrivateAttrType` | metaclass | The metaclass that implements the private-attribute machinery. |
| `PrivateWrapProxy(decorator, orig=None)` | decorator | Wrap an arbitrary decorator so it stays private-attribute-aware. |
| `_PrivateWrap` | class | Returned by `PrivateWrapProxy`; exposes `.result` and `.funcs`. |
| `prepare(name, bases, attrs, **kwds)` | function | Context manager for building private-attribute classes in a custom metaclass. |
| `postprocess(typ, temp)` | function | Finalize a class created via `prepare`. |
| `register_metaclass(metaclass)` | function | Register a custom metaclass so it can produce private-attribute classes. |
| `ensure_type(typ)` | function | Reset `tp_getattro`/`tp_setattro`/`tp_finalizer` slots on a type. |
| `ensure_metaclass(metaclass)` | function | Reset the corresponding slots on a metaclass. |

Class-definition options:

| Option | Type | Default | Meaning |
| --- | --- | --- | --- |
| `__private_attrs__` | `str` \| `Sequence[str]` | `()` | The private attribute names. |
| `private_func` | `(obj_id: int, attr_name: str) -> str` | `None` | Custom hidden-name generator. |
| `use_parents` | `bool` | `False` | Merge ancestors' private names into this class's own set. |

---

## Custom metaclasses

To combine private attributes with another metaclass (e.g. `ABCMeta`), use the
`prepare` / `postprocess` pair and register the metaclass:

```python
from abc import ABCMeta, abstractmethod
import private_attribute

class PrivateAbcMeta(ABCMeta):
    def __new__(cls, name, bases, attrs, **kwargs):
        with private_attribute.prepare(name, bases, attrs, **kwargs) as temp:
            typ = super().__new__(cls, temp.name, temp.bases, temp.attrs, **temp.kwds)
            private_attribute.postprocess(typ, temp)   # or temp.postprocess(typ)
            return typ

private_attribute.register_metaclass(PrivateAbcMeta)

class MyClass(metaclass=PrivateAbcMeta):
    __private_attrs__ = ("_a",)
    __slots__ = ()

    @abstractmethod
    def my_function(self): ...

class MyImplement(MyClass):
    __private_attrs__ = ("_a",)
    def __init__(self, value=1):
        self._a = value
    def my_function(self):
        return self._a

a = MyImplement(1)
a.my_function()          # 1
a._a                     # AttributeError: private attribute
MyClass()                # TypeError: Can't instantiate abstract class ...
```

`prepare(...)` returns a temporary object exposing `.name`, `.bases`, `.attrs`,
and `.kwds` (and, since v2.1.17, a `.postprocess(typ)` shortcut). Since v2.2.3
it is a context manager (`with`), which guarantees the internal creation lock is
released even if construction fails.

---

## Behavioral notes & limitations

- `__private_attrs__` must be a string or a sequence of strings.
- Names declared in `__slots__` cannot also be in `__private_attrs__`; however,
  private attributes may still be *set* in methods even when `__slots__` is
  defined (they live in the private-attribute storage, not `__dict__`).
- Instances of `PrivateAttrBase` (and subclasses) are **not picklable** by
  default.
- Private names are ultimately rewritten to a tuple of two hashes (unless a
  custom `private_func` is supplied).
- `_PrivateWrap` objects are transparently unwrapped back to the original
  object during class creation.
- Do **not** pass `PrivateWrapProxy` a decorator that itself returns a
  `_PrivateWrap` — this raises `TypeError`.
- A class nested inside another class cannot use the outer class's private
  attributes.
- If a parent class defines an attribute that is *not* in its
  `__private_attrs__` (nor a `PrivateAttrType` instance), a child class must not
  list that name in its own `__private_attrs__`.
- **v2.1.0** — subclass code can no longer read/write a parent's private
  attributes; re-declaring the name stores it separately.
- **v2.2.0** — a subclass must not define, in its own class body, a name that is
  private to a parent, unless it declares the name in its own
  `__private_attrs__` or opts in with `use_parents=True`.
- **v2.2.1** — `prepare` locks private-attribute-type creation so only one type
  is being built (`type.__new__`) at a time; `postprocess` unlocks it. The lock
  is released automatically on failure.
- **v2.2.3** — prefer the `with` form of `prepare`/`postprocess` to guarantee
  the lock is released.
- CPython may change `tp_getattro` / `tp_setattro` when you assign
  `__getattribute__` / `__setattr__`. Use `ensure_type` (or `ensure_metaclass`)
  to reset those slots, and avoid setting those methods on private-attribute
  classes directly.
- `register_metaclass` requires a metaclass that supports weak references.
- Do **not** set `__static_attributes__` on a private-attribute class; it is
  removed automatically.

---

## Building from source

The module is built with [scikit-build-core](https://scikit-build-core.readthedocs.io/)
and CMake (C++17). It vendors the header-only
[PicoSHA2](https://github.com/okdshin/PicoSHA2) library for SHA-256 hashing.

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

Set `PRIVATE_ATTR_KEEP_DEBUG=1` to retain debug symbols in a Linux build
(otherwise `-g0` is applied).

---

## Running the tests

```bash
pip install pytest
python -m pytest tests/ -v
```

The test suite covers attribute hiding, private methods, class-level private
state, decorator wrapping, inheritance semantics (`use_parents`), ABC/metaclass
integration, `__slots__`, and GC/cycle-collection regressions.

---

## License

[MIT](LICENSE) © 2026 Locked-chess-official.

PicoSHA2 is distributed under the MIT License (© 2017 okdshin).
