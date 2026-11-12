"""A module that provides a metaclass for creating classes with private attributes.
Private attributes are defined in the `__private_attrs__` sequence and are only visitable in class codes.
You can use the `PrivateAttrBase` as the base class to create classes with private attributes.
The attributes which are private are not on the instance's `__dict__` and cannot be accessed outside
but in the methods defined in class it is reachable.
Usage example:
```python
class MyClass(PrivateAttrBase):
    __private_attrs__ = ('_private_attr1',)
    def __init__(self):
        self._private_attr1 = 1

    @property
    def public_attr1(self):
        return self._private_attr1
```
"""
from typing_extensions import Any, TypeVar, Callable, TypedDict, Sequence, Generic, Optional, cast, Iterator
from types import FunctionType
import sys

# define the dict that must have a key "__private_attrs__" and value must be the sequence of strings
class _PrivateAttrDict(TypedDict):
    __private_attrs__: Optional[Sequence[str]]

_T = TypeVar('_T')

class _listproxy(Generic[_T]):
    "a read-only proxy of a list"
    def copy(self) -> list[_T]: ...
    def count(self, value: Any, /) -> int: ...
    def index(self, value: Any, start: int = 0, stop: int = sys.maxsize, /) -> int: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[_T]: ...
    def __getitem__(self, key: int | slice) -> Any: ...
    def __contains__(self, value: object) -> bool: ...

class _PrivateWrap(Generic[_T]):
    @property
    def result(self) -> _T:
        "the final result of decorating"
        ...

    @property
    def funcs(self) -> _listproxy[FunctionType]:
        "the original functions"
        ...

    def __getattr__(self, name: str) -> Any:
        return getattr(self.result, name)

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        if callable(self.result):
            return self.result(*args, **kwargs)
        raise TypeError(f'{self.result!r} is not callable')

    def __get__(self, instance: Any, owner: type) -> Any:
        if hasattr(self.result, "__get__"):
            descriptor = cast(Any, self.result)
            return descriptor.__get__(instance, owner)
        return self.result

_TVar = TypeVar('_TVar')

class PrivateWrapProxy(Generic[_TVar, _T]):
    """
    PrivateWrapProxy is a proxy for private attributes.
    Usage:
    ```
    from private_attribute import PrivateWrapProxy, PrivateAttrBase

    class MyClass(PrivateAttrBase):
        __private_attrs__ = ()
        @PrivateWrapProxy(decorator)
        def my_method(self): ...

        @PrivateWrapProxy(decorator)
        def my_method2(self): ...
    ```
    It returned a '_PrivateWrap' object.

    If you need to decorate more function, use like this:
    ```
    from private_attribute import PrivateWrapProxy, PrivateAttrBase

    class MyClass(PrivateAttrBase):
        __private_attrs__ = ()
        @PrivateWrapProxy(decorator)
        def my_method(self): ...

        @PrivateWrapProxy(my_method.some_decorator, my_method)
        def my_method(self): ...
    ```
    """
    def __init__(self, decorator: Callable[[_TVar], _T], orig: _PrivateWrap[Any]|None = None, /) -> None: ...
    def __call__(self, func: _TVar | _PrivateWrap[_TVar], /) -> _PrivateWrap[_T]: ...

class PrivateAttrType(type):
    "metaclass for private attributes"
    def __new__(cls, name: str, bases: tuple,
                attrs: _PrivateAttrDict, /, *,
                private_func: Callable[[int, str], str]|None = None,
                use_parents: bool = False) -> PrivateAttrType: ...

class PrivateAttrBase(metaclass=PrivateAttrType):
    "The class to help creating private attribute. It does not have any special behavior."
    __slots__ = ()
    __private_attrs__ = ()

class PrivateSuper:
    """
    Explicit cross-class access to a parent class's class-level private attribute.

    Usage: ``PrivateSuper(A, B, C, obj).name``

    - A: the type whose code is currently running (the code owner).
    - B: a parent type of A whose class-level private attribute is accessed.
    - C: the class the private variable was defined for; it must be B, A, or a
      class that is both a subclass of B and a parent of A.
    - obj: an instance of A.

    A, B and C must all be private-attribute types (instances of
    ``PrivateAttrType`` or a registered metaclass). The attribute is resolved
    from B's class-level storage for C and its ``__get__(obj, A)`` is called.
    Not for instance attribute cross-type access.
    """
    def __init__(self, A: type, B: type, C: type, obj: Any, /) -> None: ...
    def __getattr__(self, name: str) -> Any: ...

class PrivateClassSuper:
    """
    Classmethod flavour of :class:`PrivateSuper`.

    Usage: ``PrivateClassSuper(A, B, C, cls).name``

    A, B and C are as in :class:`PrivateSuper`; ``cls`` is a subclass of A. The
    attribute is resolved from B's class-level storage for C and its
    ``__get__(None, cls)`` is called.
    """
    def __init__(self, A: type, B: type, C: type, cls: type, /) -> None: ...
    def __getattr__(self, name: str) -> Any: ...


def ensure_type(typ: type, /) -> None:
    """function for custom metaclass to ensure the type is a private attributes class.
    def ensure_type(type: type) -> None:
        the function to re-inject the special-method slot descriptors and the
        tp_traverse/tp_clear wrappers after CPython has reset them.
    """
    ...
def ensure_metaclass(typ: type, /) -> None:
    """function for custom metaclass to ensure the metaclass is working.
    def ensure_metaclass(metaclass: type) -> None:
        the function to re-inject the metaclass slot descriptors and the
        tp_traverse/tp_clear wrappers.
    """
    ...

def create_new(func: Callable[..., Any] | None = None, /) -> Callable[..., Any]:
    """
    function to register the type with new automatically:

    ```
    from abc import ABCMeta
    import private_attribute

    class PrivateAbcMeta(ABCMeta):
        __new__ = private_attribute.create_new()
    ```

    also as a decorator:

    ```
    from abc import ABCMeta
    import private_attribute

    class PrivateAbcMeta(ABCMeta):
        @private_attribute.create_new
        def __new__(cls, *args, **kwargs):
            ...
    ```
    """
    ...

def create_meta_slot(name: str, /):
    """
    function to create the final slot for metaclass:

    ```
    from abc import ABCMeta
    import private_attribute

    class PrivateAbcMeta(ABCMeta):
        __new__ = private_attribute.create_new()

        __setattr__ = private_attribute.create_meta_slot("__setattr__")()
    ```

    As a decorator:

    ```
    from abc import ABCMeta
    import private_attribute

    class PrivateAbcMeta(ABCMeta):
        __new__ = private_attribute.create_new()

        @private_attribute.create_meta_slot("__setattr__")
        def __setattr__(self, name, value):
            ...
    ```
    """
    def create_register_type_slot_wrapper(callback: Callable[..., Any] | None = None) -> Callable[..., Any]: ...
    return create_register_type_slot_wrapper

def create_slot(name: str, /):
    """
    function to create the final slot for private class:

    ```
    import private_attribute

    class MyClass(private_attribute.PrivateAttrBase):
        __setattr__ = private_attribute.create_slot("__setattr__")()

    ```
    As a decorator:

    ```
    import private_attribute

    class MyClass(private_attribute.PrivateAttrBase):
        @private_attribute.create_slot("__setattr__")
        def __setattr__(self, name, value):
            ...

    ```
    """
    def create_private_type_slot_wrapper(func: Callable[..., Any] | None = None) -> Callable[..., Any]: ...
    return create_private_type_slot_wrapper
