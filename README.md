# Private Attribute (c++ implementation)

## Introduction

This package provide a way to create the private attribute like "C++" does.

## All API

```python
from private_attribute import (PrivateAttrBase, PrivateWrapProxy)      # 1 Import public API

def my_generate_func(obj_id, attr_name):                           # 2 Optional: custom name generator
    return f"_hidden_{obj_id}_{attr_name}"

class MyClass(PrivateAttrBase, private_func=my_generate_func):     # 3 Inherit + optional custom generator
    __private_attrs__ = ['a', 'b', 'c', 'result', 'conflicted_name']  # 4 Must declare all private attrs

    def __init__(self):
        self.a = 1
        self.b = 2
        self.c = 3
        self.result = 42                    # deliberately conflicts with internal names

    # Normal methods can freely access private attributes
    def public_way(self):
        print(self.a, self.b, self.c)

    # Real-world case: method wrapped by multiple decorators
    @PrivateWrapProxy(memoize())                                   # 5 Apply any decorator safely
    @PrivateWrapProxy(login_required())                            # 5 Stack as many as needed
    @PrivateWrapProxy(rate_limit(calls=10))                        # 5
    def expensive_api_call(self, x):                               # First definition (will be wrapped)
        def inner(...):
            return some_implementation(self.a, self.b, self.c, x)
        inner(...)
        return heavy_computation(self.a, self.b, self.c, x)

    # Fix decorator order + resolve name conflicts
    @PrivateWrapProxy(expensive_api_call.result.name2, expensive_api_call)    # 6 Chain .result to push decorators down
    @PrivateWrapProxy(expensive_api_call.result.name1, expensive_api_call)    # 6 Resolve conflict with internal names
    def expensive_api_call(self, x):         # Final real implementation
        return heavy_computation(self.a, self.b, self.c, x)


# ====================== Usage ======================
obj = MyClass()
obj.public_way()                    # prints: 1 2 3

print(hasattr(obj, 'a'))            # False – truly hidden from outside
print(obj.expensive_api_call(10))   # works with all decorators applied
```

| # | API | Purpose | Required? |
|---|----------------------------------------|-------------------------------------------------------|-----------|
| 1 | PrivateAttrBase | Base class – must inherit | Yes |
| 1 | PrivateWrapProxy  | Decorator wrapper for arbitrary decorators  | When needed |
| 2 | private_func=callable  | Custom hidden-name generator  | Optional |
| 3 | Pass private_func in class definition | Same as above   | Optional |
| 4 | \_\_private_attrs\_\_ list | Declare which attributes are private | Yes |
| 5 | @PrivateWrapProxy(...) | Make any decorator compatible with private attributes | When needed |
| 6 | method.result.xxx chain + dummy wrap | Fix decorator order and name conflicts | When needed |

## Usage

This is a simple usage about the module:

```python
from private_attribute import PrivateAttrBase

class MyClass(PrivateAttrBase):
    __private_attrs__ = ['a', 'b', 'c']
    def __init__(self):
        self.a = 1
        self.b = 2
        self.c = 3

    def public_way(self):
        print(self.a, self.b, self.c)

obj = MyClass()
obj.public_way()  # (1, 2, 3)

print(hasattr(obj, 'a'))  # False
print(hasattr(obj, 'b'))  # False
print(hasattr(obj, 'c'))  # False
```

All of the attributes in `__private_attrs__` will be hidden from the outside world, and stored by another name.

You can use your function to generate the name. It needs the id of the obj and the name of the attribute:

```python
def my_generate_func(obj_id, attr_name):
    return some_string

class MyClass(PrivateAttrBase, private_func=my_generate_func):
    __private_attrs__ = ['a', 'b', 'c']
    def __init__(self):
        self.a = 1
        self.b = 2
        self.c = 3

    def public_way(self):
        print(self.a, self.b, self.c)

obj = MyClass()
obj.public_way()  # (1, 2, 3)

```

If the method will be decorated, the `property`, `classmethod` and `staticmethod` will be supported.
For the other, you can use the `PrivateWrapProxy` to wrap the function:

```python
from private_attribute import PrivateAttrBase, PrivateWrapProxy

class MyClass(PrivateAttrBase):
    __private_attrs__ = ['a', 'b', 'c']
    @PrivateWrapProxy(decorator1())
    @PrivateWrapProxy(decorator2())
    def method1(self):
        ...

    @method1.attr_name
    @PrivateWrapProxy(lambda _: _) # use empty function to wrap
    def method1(self):
        ...

    @PrivateWrapProxy(decorator3())
    def method2(self):
        ...

    @method2.attr_name
    @PrivateWrapProxy(lambda _: _)
    def method2(self):
        ...


```

The `PrivateWrapProxy` is a decorator, and it will wrap the function with the decorator. When it decorates the method, it returns a `_PrivateWrap` object.

The `_PrivateWrap` has the public api `result`. It returns the original decoratored result.

```python
from private_attribute import PrivateAttrBase, PrivateWrapProxy

class MyClass(PrivateAttrBase):
    __private_attrs__ = ['a', 'b', 'c']
    @PrivateWrapProxy(decorator1())
    @PrivateWrapProxy(decorator2())
    def method1(self):
        ...

    @PrivateWrapProxy(method1.result.conflict_attr_name1, method1) # Use the argument "method1" to save old func
    def method1(self):
        ...

    @PrivateWrapProxy(method1.result.conflict_attr_name2, method1)
    def method1(self):
        ...

    @PrivateWrapProxy(decorator3())
    def method2(self):
```

## Notes

- All of the private attributes class must contain the `__private_attrs__` attribute.
- The `__private_attrs__` attribute must be a sequence of strings.
- You cannot define the name which in `__slots__` to `__private_attrs__`.
- When you define `__slots__` and `__private_attrs__` in one class, the attributes in `__private_attrs__` can also be defined in the methods, even though they are not in `__slots__`.
- All of the object that is the instance of the class "PrivateAttrBase" or its subclass are default to be unable to be pickled.
- Finally the attributes' names in `__private_attrs__` will be change to a tuple with two hash.
- Finally the `_PrivateWrap` object will be recoveried to the original object.
- One class defined in another class cannot use another class's private attribute.
- One parent class defined an attribute which not in `__private_attrs__` or not a `PrivateAttrType` instance, the child class shouldn't contain the attribute in its `__private_attrs__`.

## License

MIT

## Requirement

This package require the c++ module "picosha2" to compute the sha256 hash.

## Support

Now it doesn't support "PyPy".
