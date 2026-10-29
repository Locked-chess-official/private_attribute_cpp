# Inheritance & `use_parents`

Private attributes follow strict **per-declaring-class** ownership rules. This
is what gives them real privacy, but it also means you need to understand how
they behave across an inheritance hierarchy.

## Subclasses cannot see parent privacy

Since **v2.1.0**, the code of a subclass **cannot** read or write a parent's
private attribute. A parent's private attribute is reachable only from:

- the parent's own code, or
- a class that re-declares the same name in its own `__private_attrs__`.

```python
from private_attribute import PrivateAttrBase

class Parent(PrivateAttrBase):
    __private_attrs__ = ["_secret"]

    def __init__(self):
        self._secret = 1

    def get_secret(self):
        return self._secret

class Child(Parent):
    __private_attrs__ = []

    def read_parent_secret(self):
        return self._secret   # AttributeError

obj = Child()
obj.get_secret()              # 1  — parent code sees parent value
```

## Re-declaring the same name

When a subclass re-declares a name in its own `__private_attrs__`, the two are
stored **separately** — instance attributes per declaring class, class-level
attributes per class. The parent's value is never overwritten by the child.

```python
class Child(Parent):
    __private_attrs__ = ["_secret"]

    def __init__(self):
        super().__init__()
        self._secret = 99     # stored in Child's own storage

    def read_own(self):
        return self._secret

obj = Child()
obj.get_secret()    # 1  — parent code sees the parent's value
obj.read_own()      # 99 — child code sees its own value
```

## `use_parents=True`

Since **v2.2.0**, you can opt in to merging every ancestor's private names into
the subclass's own set with `use_parents=True`:

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
        self._secret = 99      # stored in Child's own storage

    def read_own(self):
        return self._secret

obj = Child()
obj.get_secret()   # 1  — parent code sees the parent's value
obj.read_own()     # 99 — child code sees its own value
```

Key points:

- The subclass's code may now read/write the parent's private names, but the
  values live in the **subclass's own storage** — they never overwrite the
  parent's own values.
- The Python-level `__private_attrs__` tuple is left unchanged; only the
  internal private-attribute name set is merged.
- Default is `False`.

## Namespace restriction (v2.2.0)

A subclass must **not** define, in its own class body, a name that is private to
a parent — unless it declares that name in its own `__private_attrs__` or opts in
with `use_parents=True`:

```python
class Parent(PrivateAttrBase):
    __private_attrs__ = ["_secret"]

class Bad(Parent):              # TypeError
    __private_attrs__ = []
    _secret = 999

class Good(Parent, use_parents=True):
    __private_attrs__ = []
    _secret = 999               # OK — stored in Good's own storage
```

## Summary table

| Version | Behavior |
| --- | --- |
| ≤ 2.0 | Subclasses could access parent private attributes. |
| 2.1.0 | Subclass code cannot access parent private attributes; re-declaring stores separately. |
| 2.2.0 | Namespace restriction on parent-private names; `use_parents` added. |
