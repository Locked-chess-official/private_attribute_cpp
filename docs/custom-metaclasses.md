# Custom Metaclasses

To combine private attributes with another metaclass (for example `ABCMeta`),
use the `prepare` / `postprocess` pair and register the metaclass.

## Example: private attributes + ABC

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
```

Now a class can behave as both an ABC and a private-attribute class:

```python
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

MyClass()                # TypeError: Can't instantiate abstract class MyClass
                         # without an implementation for abstract method 'my_function'
```

## How it works

- `prepare(name, bases, attrs, **kwds)` is a context manager (since v2.2.3)
  that returns a temporary object holding `.name`, `.bases`, `.attrs`, and
  `.kwds`.
- Call your metaclass's `super().__new__` with those values, then finalize with
  `postprocess(typ, temp)` (or `temp.postprocess(typ)`).
- `register_metaclass` tells the module about your metaclass so the private
  attribute machinery is installed correctly.

!!! tip "Locking"

    Since v2.2.1, `prepare` takes an internal creation lock so only one
    private-attribute type is being built (`type.__new__`) at a time.
    `postprocess` releases it. The lock is released automatically if
    construction fails. Prefer the `with` form (v2.2.3+) to guarantee release.

## `ensure_type` / `ensure_metaclass`

CPython may change `tp_getattro` / `tp_setattro` when you assign
`__getattribute__` / `__setattr__`. Use `ensure_type` (for types) or
`ensure_metaclass` (for metaclasses) to reset those slots, and avoid setting
those dunder methods on private-attribute classes directly.

```python
private_attribute.ensure_type(SomeClass)
private_attribute.ensure_metaclass(SomeMeta)
```
