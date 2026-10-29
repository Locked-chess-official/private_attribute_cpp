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
| 2.1.17 | `_PrivateTemp` gained a `.postprocess(typ)` shortcut. |
| 2.2.0 | Subclass must not define a parent-private name in its own body unless declared in its own `__private_attrs__` or `use_parents=True`. `use_parents` added. |
| 2.2.1 | `prepare` takes an internal creation lock; `postprocess` releases it. Auto-released on failure. |
| 2.2.3 | `prepare`/`postprocess` support the `with` statement. |

## CPython slot caveats

- CPython may change `tp_getattro`, `tp_setattro`, and related slots when you
  change `__getattribute__`, `__setattr__`, etc. If you are concerned, use
  `ensure_type` (or `ensure_metaclass`) to reset those slots, and avoid setting
  those dunder methods on private-attribute classes directly.
- `register_metaclass` must be called with a metaclass that supports weakrefs.
- Do **not** set `__static_attributes__` on a private-attribute class — it will
  be removed.
