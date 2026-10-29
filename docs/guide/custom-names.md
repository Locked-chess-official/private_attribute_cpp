# Custom name generation

By default, private values are stored under generated hash names. You can
control how the storage name is derived by supplying a `private_func` callable.
It receives the **object id** and the **attribute name**:

```python
def my_generate_func(obj_id: int, attr_name: str) -> str:
    return f"_hidden_{obj_id}_{attr_name}"
```

## Passing `private_func`

There are two equivalent ways to attach it:

```python
from private_attribute import PrivateAttrBase

def my_generate_func(obj_id, attr_name):
    return f"_hidden_{obj_id}_{attr_name}"

# Option A: keyword in the class definition
class MyClass(PrivateAttrBase, private_func=my_generate_func):
    __private_attrs__ = ["secret"]

    def __init__(self):
        self.secret = 42

    def read(self):
        return self.secret
```

```python
# Option B: on the base class itself
class Base(PrivateAttrBase, private_func=my_generate_func):
    ...

class MyClass(Base):
    __private_attrs__ = ["secret"]
```

Either way, `private_func` is optional; the default generates a tuple of two
SHA-256 hashes.

!!! warning "Keep a strong reference"

    The metaclass stores a reference to `private_func`, so a *local* function
    used only for one class is safe to drop afterwards — the module handles its
    lifetime correctly (a past bug here caused a segfault on `gc.collect()`,
    now fixed). Still, for clarity prefer module-level functions.
