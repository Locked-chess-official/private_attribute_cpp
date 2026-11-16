# Behavioral Notes & Limitations

## General

- `__private_attrs__` must be a **string** or a **sequence of strings**.
- Names declared in `__slots__` cannot also be in `__private_attrs__`; however,
  private attributes may still be *set* in methods even when `__slots__` is
  defined (they live in private-attribute storage, not `__dict__`).
- Instances of `PrivateAttrBase` (and its subclasses) are **not picklable** by
  default.
- Private names are ultimately rewritten to a **tuple of two hashes** (unless a
  custom `private_func` is supplied).
- `_PrivateWrap` objects are transparently unwrapped back to the original object
  during class creation.
- Do **not** pass `PrivateWrapProxy` a decorator that itself returns a
  `_PrivateWrap` — this raises `TypeError`.
- A class nested inside another class cannot use the outer class's private
  attributes.
- If a parent class defines an attribute that is *not* in its
  `__private_attrs__` (nor a `PrivateAttrType` instance), a child class must not
  list that name in its own `__private_attrs__`.

## Version changelog (behavioral)

| Version | Change |
| --- | --- |
| 2.1.0 | Subclass code can no longer access a parent's private attributes; re-declaring the name stores it separately (instance attrs per declaring class, class attrs per class). |
| 2.1.4 | `__private_attrs__` became optional. |
| 2.2.0 | Subclass must not define a parent-private name in its own body unless declared in its own `__private_attrs__` or `use_parents=True`. `use_parents` added. |
| 3.0.0 | Interception moved from the `tp_getattro`/`tp_setattro`/`tp_finalize` slots to C-struct slot descriptors; added `PrivateSuper`/`PrivateClassSuper`. |
| unreleased (after 3.0.4) | Added `PrivateObjSuper` for read-only per-instance cross-class access. Fixed stale private-class cache entries left behind after a type is collected. |
| unreleased (after 3.0.4) | Collecting a class created by a private class used as a metaclass no longer prints an ignored `TypeError` from `__del__`. |

## CPython slot caveats

- In v3 the interception lives in C-struct slot descriptors installed as
  `__getattribute__` / `__getattr__` / `__setattr__` / `__delattr__` /
  `__del__` (plus `__call__` on metaclasses); only `tp_traverse` / `tp_clear`
  are set directly. CPython may reset these descriptors when you change
  `__getattribute__`, `__setattr__`, etc. If you are concerned, use
  `ensure_type` (or `ensure_metaclass`) to re-inject them, and avoid setting
  those dunder methods on private-attribute classes directly. To install custom
  slot behaviour, prefer `create_slot` (for classes) or `create_meta_slot` (for
  metaclasses).
- Do **not** set `__static_attributes__` on a private-attribute class — it will
  be removed.

## `PrivateObjSuper`

- Read-only. Assignment and `del` raise `AttributeError` and leave the parent's
  value unchanged.
- Reads only `parent_class`'s per-instance storage for `obj`. A class attribute
  of the same name is invisible here (use `PrivateSuper`). An ancestor's
  instance value is also invisible unless that ancestor is passed as
  `parent_class`.
- A data descriptor installed on the class does **not** take precedence over a
  value already stored on the instance. For a normal instance the stored object
  is returned as-is, descriptor or not. `__get__(None, obj)` runs only when
  `obj` itself is a class.
- The type cannot be subclassed, and its instances cannot be weak-referenced.
- `dir()` on a `PrivateObjSuper` does not list the private names.
