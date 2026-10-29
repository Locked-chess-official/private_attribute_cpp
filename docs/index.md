# private_attribute_cpp

True C++-style **private attributes** for Python, enforced at the C level.

`private_attribute` is a C++ extension module that lets you declare attributes
accessible **only from inside the class that owns them** — genuinely hidden from
the outside world, from subclasses, and from `hasattr()`, `dir()`, and
`obj.__dict__`.

Unlike Python's name-mangling (`_Class__name`) and the leading-underscore
convention, these attributes cannot be reached at all from outside the declaring
class's own code.

<div class="grid cards" markdown>

- :material-lock: **Enforced at the C level**

    Attribute access is rewritten in `tp_getattro` / `tp_setattro`, not emulated
    in Python.

- :material-eye-off: **Truly hidden**

    Not in `__dict__`. Invisible to `hasattr()`, `getattr()`, and `dir()`.

- :material-family-tree: **Inheritance-safe**

    Subclasses cannot see a parent's private state unless you explicitly opt in.

- :material-package-variant: **Zero dependencies**

    A single compiled extension vendoring the header-only PicoSHA2 for hashing.

</div>

---

## A taste

```python
from private_attribute import PrivateAttrBase

class BankAccount(PrivateAttrBase):
    __private_attrs__ = ["_balance"]

    def __init__(self, balance):
        self._balance = balance

    def deposit(self, amount):
        self._balance += amount       # ✅ accessible inside the class

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

Continue with the [Quick Start](quickstart.md) or dive into the
[Guide](guide/index.md).

---

## At a glance

| | |
| --- | --- |
| **Language** | C++17, CPython C-API |
| **Python** | ≥ 3.10 (CPython only, including free-threaded `t` builds) |
| **Platforms** | Linux, macOS, Windows |
| **License** | [MIT](license.md) |
