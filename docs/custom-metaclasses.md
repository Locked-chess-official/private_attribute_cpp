# Custom Metaclasses

To combine private attributes with another metaclass (for example `ABCMeta`),
put a `create_new` slot directly in the metaclass. `create_new` takes care of
registration and control automatically.

## Example: private attributes + ABC

```python
from abc import ABCMeta, abstractmethod
import private_attribute

class PrivateAbcMeta(ABCMeta):
    __new__ = private_attribute.create_new()
```

`create_new()` returns a `__new__` slot descriptor. When it is placed in the
metaclass body, `__set_name__` registers the metaclass automatically, and every
class built through this metaclass is finalized as a private-attribute class.

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

## `create_new` as a decorator

`create_new` may also be used as a decorator. The wrapped function is called in
place of the default `type.__new__`, and the resulting type is finalized
afterwards:

```python
from abc import ABCMeta
import private_attribute

class PrivateAbcMeta(ABCMeta):
    @private_attribute.create_new
    def __new__(cls, name, bases, attrs, **kwargs):
        return super().__new__(cls, name, bases, attrs, **kwargs)
```

With no argument (`create_new()`), the default `super().__new__` is used and no
customisation hook is applied.

## Custom slots on the metaclass

The special-method slots of a registered metaclass (its `__getattribute__`,
`__getattr__`, `__setattr__`, `__delattr__`, `__del__`, and `__call__`) are
installed with `create_meta_slot`. Like `create_new`, it works both as an empty
slot and as a decorator:

```python
from abc import ABCMeta
import private_attribute

class PrivateAbcMeta(ABCMeta):
    __new__ = private_attribute.create_new()

    # empty slot — the default `super()` behaviour is used
    __setattr__ = private_attribute.create_meta_slot("__setattr__")()

    # decorator form — the callback replaces the default `super()` behaviour
    @private_attribute.create_meta_slot("__getattr__")
    def __getattr__(cls, name):
        ...
```

## Custom slots on private-attribute classes

The corresponding slots of a private-attribute *class* (`__getattribute__`,
`__getattr__`, `__setattr__`, `__delattr__`, `__del__`) are installed with
`create_slot`:

```python
import private_attribute

class MyClass(private_attribute.PrivateAttrBase):
    __setattr__ = private_attribute.create_slot("__setattr__")()

    @private_attribute.create_slot("__delattr__")
    def __delattr__(self, name):
        ...
```

## `ensure_type` / `ensure_metaclass`

In v3 the interception lives in C-struct slot descriptors installed as
`__getattribute__` / `__getattr__` / `__setattr__` / `__delattr__` / `__del__`
(plus `__call__` on metaclasses); only `tp_traverse` / `tp_clear` are set
directly. CPython may reset these descriptors when you assign
`__getattribute__` / `__setattr__` (and friends). Use `ensure_type` (for types)
or `ensure_metaclass` (for metaclasses) to re-inject them, and avoid setting
those dunder methods on private-attribute classes directly.

```python
private_attribute.ensure_type(SomeClass)
private_attribute.ensure_metaclass(SomeMeta)
```
