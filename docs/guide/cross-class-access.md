# Cross-class access

`private_attribute` hides each private attribute from every class except the one
that declared it. Two helpers let you *explicitly* reach a parent's private
value from a subclass. They read different storage:

- **Class-level** — `PrivateSuper` and `PrivateClassSuper` (since **v3.0.0**).
  `PrivateSuper` resolves the attribute and calls `__get__(obj, A)`.
  `PrivateClassSuper` is the `@classmethod` flavour and calls `__get__(None, cls)`.
- **Per-instance** — `PrivateObjSuper`. It reads the value `parent_class` stored
  on one object. It does not read class attributes.

!!! warning

    `PrivateSuper` and `PrivateClassSuper` are for **class-level** private
    attributes only. Instance values go through `PrivateObjSuper`, and that
    helper is read-only.

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

## Per-instance (`PrivateObjSuper`)

`PrivateObjSuper` is how subclass code reads a **parent's per-instance** private
attribute without touching the subclass's own storage for the same name.
Re-declaring the name, or opting in with `use_parents=True`, still stores the
subclass's writes separately; this helper reaches the parent's map on purpose.

### Signature

```python
private_attribute.PrivateObjSuper(now_class, parent_class, obj).name
```

| Argument | Meaning |
| --- | --- |
| `now_class` | The private-attribute type whose code is currently running. |
| `parent_class` | A private-attribute parent of `now_class`, not `now_class` itself. The value is read from this class's instance storage. |
| `obj` | An instance of `now_class`. An instance of a further subclass is accepted. |

Exactly three positional arguments. Keyword arguments raise `TypeError`.

### Example

```python
import private_attribute

class Parent(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_secret"]

    def __init__(self):
        self._secret = 1

    def get_secret(self):
        return self._secret

class Child(Parent):
    __private_attrs__ = ["_secret"]

    def __init__(self):
        super().__init__()
        self._secret = 99

    def read_own(self):
        return self._secret

    def read_parent(self):
        return private_attribute.PrivateObjSuper(Child, Parent, self)._secret

obj = Child()
obj.get_secret()     # 1
obj.read_own()       # 99
obj.read_parent()    # 1
```

The same split holds with `use_parents=True`: the child's `self._secret = 99`
does not overwrite the value `Parent.__init__` stored.

### What is read

The storage key is the one `parent_class` itself uses for that instance: the
object id plus the attribute name, passed through `parent_class`'s
`private_func` when the parent defines one, otherwise the default generator.
Only `parent_class`'s own per-instance map is consulted.

- A name that is not private on `parent_class` or on any of its
  private-attribute ancestors raises
  `AttributeError: '<Parent>' has no private attribute '<name>'`.
- A private name that `parent_class` never stored on this instance raises
  `AttributeError: 'PrivateObjSuper' object has no attribute '<name>'`.
  That includes a name that exists only as a **class** attribute (read it with
  `PrivateSuper`) and a name stored by an **ancestor** of `parent_class`.
  Pass the class that wrote the value. An intermediate class is a valid
  `parent_class`, but it does not forward the read to its parents.
- For a normal instance the stored object is returned as-is. A descriptor
  object sitting in the instance slot is not bound, and a data descriptor on
  the class does not take precedence over the instance value.
- When `obj` is itself a class (`now_class` is a metaclass) and the stored
  value has `__get__`, it is called as `__get__(None, obj)`. That call can
  re-enter private-attribute access on the same class.

`hasattr` from inside `now_class` reports whether the instance slot is present.
Outside `now_class` it is `False`. `dir()` does not list the private names.

### Ownership and validation

The proxy may be constructed anywhere the three arguments are valid. Reading an
attribute is allowed only while the running code object belongs to `now_class`:
a method, a function or lambda nested inside one, a property, a classmethod, a
staticmethod, or a `PrivateWrapProxy` wrapper defined in that class. From
anywhere else — module level, the parent's methods, a function defined outside
the class and then called or assigned onto it — resolution raises
`AttributeError: cannot use 'PrivateObjSuper' outside the class`.

Construction checks, in order:

- `now_class` and `parent_class` must be types
  (`TypeError: now_class must be a type` / `parent_class must be a type`).
- Both must be private-attribute classes
  (`TypeError: now_class must be private class` /
  `parent_class must be private class`).
- `parent_class` must appear in `now_class`'s private-attribute parents
  (`TypeError: parent_class must be parent of now_class`). Passing
  `now_class` itself fails this check.
- `obj` must be an instance of `now_class`
  (`TypeError: obj must be an instance of now_class`).

The type cannot be subclassed, and a `PrivateObjSuper` instance cannot be
weak-referenced. It participates in cyclic GC, so a cycle that runs through an
ordinary instance `__dict__` is collectable.

### Writes

There is no setter. `proxy.name = ...` and `del proxy.name` raise
`AttributeError` (`'private_attribute.PrivateObjSuper' object has no attribute
'...'`) and do not modify the parent's value.

### `private_func`

A `private_func` on `parent_class` is used to build the storage key, with the
**instance** id (not the class id). If that callable raises, the exception
propagates. If it returns a non-string, the result is
`TypeError: private_func function must return a string, not '...'`.
