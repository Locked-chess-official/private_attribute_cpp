# Cross-class access

`private_attribute` hides each private attribute from every class except the one
that declared it. Since **v3.0.0**, two helpers let you *explicitly* reach a
parent class's **class-level** private attribute from a subclass — including the
value the parent stored "for" an intermediate class in the hierarchy.

- `PrivateSuper` — the instance flavour; resolves the attribute and calls
  `__get__(obj, A)`.
- `PrivateClassSuper` — the `@classmethod` flavour; resolves the same storage and
  calls `__get__(None, cls)`.

!!! warning

    These are for **class-level private attributes only**. They do **not** give
    cross-type access to instance attributes.

## Signatures

```python
private_attribute.PrivateSuper(A, B, C, obj).name
private_attribute.PrivateClassSuper(A, B, C, cls).name
```

| Argument | `PrivateSuper` | `PrivateClassSuper` | Meaning |
| --- | --- | --- | --- |
| `A` | type | type | The type whose code is currently running (the code owner). |
| `B` | type | type | A parent type of `A` whose class-level private attribute is accessed. |
| `C` | type | type | The class the private value was defined for; `B`, `A`, or a class that is both a subclass of `B` and a parent of `A`. |
| `obj` | instance of `A` | — | The instance to pass to `__get__(obj, A)`. |
| `cls` | — | subclass of `A` | The class to pass to `__get__(None, cls)`. |

`A`, `B`, and `C` must all be private-attribute types (instances of
`PrivateAttrType` or a registered metaclass).

## Example

```python
import private_attribute

class Grand(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_g"]
    _g = "grand"

    @classmethod
    def set_g(cls, value):
        cls._g = value

class Mid(Grand):
    __private_attrs__ = ["_m"]
    _m = "mid"

class Leaf(Mid, use_parents=True):
    __private_attrs__ = []

    def read_grand_shadow(self):
        # A=Leaf, B=Grand, C=Mid (subclass of B, parent of A)
        return private_attribute.PrivateSuper(Leaf, Grand, Mid, self)._g

    @classmethod
    def read_grand_shadow_cls(cls):
        return private_attribute.PrivateClassSuper(Leaf, Grand, Mid, cls)._g
```

Running `Grand`'s code on `Mid` writes a value into the storage that `Grand`
keeps *for* `Mid`; `Leaf` can then read it explicitly:

```python
Mid.set_g("written-on-mid")

Leaf().read_grand_shadow()      # "written-on-mid"
Leaf.read_grand_shadow_cls()    # "written-on-mid"
```

## What `C` can be

The `C` argument selects which storage slot to read from `B`:

- `C is B` — reads `B`'s own class-body storage.
- `C is A` — reads the shadow `B` holds for the running class.
- otherwise — reads the shadow `B` holds for an intermediate subclass of `B`
  that is also a parent of `A`.

```python
class Child(Grand, use_parents=True):
    __private_attrs__ = []
    def read(self):
        # C == B: Grand's own class-body value
        return private_attribute.PrivateSuper(Child, Grand, Grand, self)._g
```

## Ownership and validation

Access is only permitted from `A`'s own code. From anywhere else, resolving the
attribute raises `AttributeError` (the running code is not `A`'s code), even
though the `PrivateSuper` object itself can be constructed.

Construction is also validated eagerly:

- `B` must be a parent type of `A` (`ValueError` otherwise).
- `A`, `B`, `C` must all be private-attribute types (`ValueError` otherwise).
- `C` must be `B`, `A`, or a subclass of `B` that is a parent of `A`
  (`ValueError` otherwise).
- The final argument must be an instance of `A` (`TypeError` otherwise) for
  `PrivateSuper`, or a subclass of `A` (`TypeError` otherwise) for
  `PrivateClassSuper`.

## Descriptor handling

When the resolved attribute is a descriptor, its `__get__` is invoked with the
appropriate binding. A plain (non-descriptor) value is returned directly:

```python
class P(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_n"]
    _n = 7

    @classmethod
    def set_n(cls, v):
        cls._n = v

class C(P, use_parents=True):
    __private_attrs__ = []
    def read(self):
        return private_attribute.PrivateSuper(C, P, C, self)._n

C.set_n(11)
C().read()   # 11
```
