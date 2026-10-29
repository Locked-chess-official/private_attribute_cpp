# Quick Start

## Minimal example

```python
from private_attribute import PrivateAttrBase

class BankAccount(PrivateAttrBase):
    __private_attrs__ = ["_balance"]

    def __init__(self, balance):
        self._balance = balance

    def deposit(self, amount):
        self._balance += amount      # ✅ accessible inside the class

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

## The three ingredients

1. **Inherit `PrivateAttrBase`.**
2. **Declare** the private names in `__private_attrs__`.
3. **Use them normally** inside the class — they're hidden everywhere else.

```python
from private_attribute import PrivateAttrBase

class MyClass(PrivateAttrBase):
    __private_attrs__ = ["a", "b", "c"]   # a string, or a sequence of strings

    def __init__(self):
        self.a = 1
        self.b = 2
        self.c = 3

    def show(self):
        print(self.a, self.b, self.c)     # visible inside

obj = MyClass()
obj.show()                                # 1 2 3

print(hasattr(obj, "a"))                  # False
print(hasattr(obj, "b"))                  # False
print(hasattr(obj, "c"))                  # False
```

## What can be private?

- **Instance attributes** — the common case above.
- **Class attributes** — usable from `@classmethod`.
- **Methods** — declare the method's name in `__private_attrs__` and it can only
  be called from the class's own code.

```python
from private_attribute import PrivateAttrBase

class Counter(PrivateAttrBase):
    __private_attrs__ = ["_count", "_increment"]

    _count = 0                            # class-level private state

    def _increment(self):                 # private method
        return 1

    def tick(self):
        self._count += self._increment()  # both accessible inside

    @classmethod
    def get_count(cls):
        return cls._count

c = Counter()
c.tick()
print(c.get_count())     # 1
print(hasattr(c, "_count"))       # False
print(hasattr(c, "_increment"))   # False
```

---

Next: learn about [declaring attributes](guide/declaring.md), or see the full
[API reference](api.md).
