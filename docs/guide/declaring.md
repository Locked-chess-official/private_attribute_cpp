# Declaring private attributes

List the private names in the `__private_attrs__` class attribute. It accepts a
single string or a sequence of strings.

```python
from private_attribute import PrivateAttrBase

class MyClass(PrivateAttrBase):
    __private_attrs__ = ["a", "b", "c"]

    def __init__(self):
        self.a = 1
        self.b = 2
        self.c = 3

    def show(self):
        print(self.a, self.b, self.c)   # (1, 2, 3)

obj = MyClass()
obj.show()                              # 1 2 3

print(hasattr(obj, "a"))                # False
print(hasattr(obj, "b"))                # False
print(hasattr(obj, "c"))                # False
```

## What happens under the hood

When a name is declared private:

1. Its value is **not** stored in the instance's `__dict__`.
2. It is stored under a generated, collision-resistant name — by default a tuple
   of two SHA-256 hashes.
3. Reads and writes are intercepted by `tp_getattro` / `tp_setattro` and routed
   to the private storage *only* when the accessing code belongs to the
   declaring class.

This is why the attribute is invisible to `hasattr()`, `getattr()`, and `dir()`
from outside — the value simply isn't under the public name.

## Class-level private state

Private attributes can live on the **class** itself, which is useful with
`@classmethod`:

```python
from private_attribute import PrivateAttrBase

class Config(PrivateAttrBase):
    __private_attrs__ = ["_api_key"]

    _api_key = "secret"

    @classmethod
    def get_key(cls):
        return cls._api_key

print(Config.get_key())        # secret
print(Config._api_key)         # AttributeError
```

## Private methods

Methods are attributes too. Declare a method's name and it becomes callable only
from the declaring class's own code:

```python
from private_attribute import PrivateAttrBase

class Widget(PrivateAttrBase):
    __private_attrs__ = ["_secret_method", "_a"]

    def __init__(self, a):
        self._a = a

    def _secret_method(self):
        return self._a

    def public_method(self):
        return self._secret_method()

w = Widget(1)
print(w.public_method())       # 1
w._secret_method()             # AttributeError
```

## Interplay with `__slots__`

- Names declared in `__slots__` **cannot** also appear in `__private_attrs__`.
- However, when `__slots__` is defined, private attributes may still be *set*
  inside methods — they live in the private-attribute storage, not in `__dict__`
  or a slot.

```python
from private_attribute import PrivateAttrBase

class Slotted(PrivateAttrBase):
    __private_attrs__ = ["_secret"]
    __slots__ = ["public"]

    def __init__(self, secret, public):
        self._secret = secret    # OK — stored in private storage
        self.public = public

    def read(self):
        return self._secret
```

!!! note

    The `__private_attrs__` attribute itself is rewritten to a tuple of tuples
    after class creation, reflecting the generated storage names.
