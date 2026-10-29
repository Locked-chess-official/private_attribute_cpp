# Decorated methods

## The problem

Decorators such as `@lru_cache`, `@retry`, or a custom wrapper return a *new*
function object whose bytecode is not recognized as belonging to your class.
Because `private_attribute` routes access by identifying which code object is
doing the access, a plain wrapper would lose access to private attributes.

`property`, `classmethod`, and `staticmethod` are handled natively and need no
special treatment. For **everything else**, wrap the decorator in
`PrivateWrapProxy`.

## Basic usage

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
decorator application.

## `.result` and `.funcs`

The `_PrivateWrap` object exposes two public attributes:

| Attribute | Meaning |
| --- | --- |
| `.result` | The final decorated result. |
| `.funcs`  | A read-only list proxy (`_listproxy`) of the original (pre-decoration) functions. |

It is also **transparent**: it forwards `__getattr__`, `__call__`, and the
descriptor `__get__` to `.result`, so callers interact with the wrapped method
as if it were the plain decorated function.

## Stacking, ordering, and name conflicts

To stack multiple decorators, resolve name conflicts, or fix decorator
ordering, chain `.result.<attr>` onto the wrapped method and pass the method
itself as the second argument. That second argument saves the previously
captured functions:

```python
from private_attribute import PrivateAttrBase, PrivateWrapProxy

class MyClass(PrivateAttrBase):
    __private_attrs__ = ["a", "b", "c"]

    @PrivateWrapProxy(deco1)
    def method1(self):
        return self.a

    @PrivateWrapProxy(method1.result.name1, method1)
    @PrivateWrapProxy(method1.result.name2, method1)
    def method1(self):        # final implementation; earlier ones are preserved
        return self.b

    @PrivateWrapProxy(deco2)
    def method2(self):
        return self.c
```

Here's how to read it:

- Each `@PrivateWrapProxy(method1.result.xxx, method1)` line takes the method as
  currently built (`method1`), re-wraps it, and exposes the result under a new
  name (`xxx`) on `method1.result`.
- The final `def method1` is the "real" implementation.
- Because each decoration layer saves the previous function, none of the earlier
  definitions are lost.

!!! warning

    Do **not** pass `PrivateWrapProxy` a decorator that itself returns a
    `_PrivateWrap` — this raises `TypeError`.
