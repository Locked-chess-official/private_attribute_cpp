"""A module that provides a metaclass for creating classes with private attributes.
Private attributes are defined in the `__private_attrs__` sequence and are only
You can use the `PrivateAttrBase` metaclass to create classes with private attributes.
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
from typing import Any, TypeVar, Callable, TypedDict, Sequence, Generic
from types import FunctionType

# define the dict that must have a key "__private_attrs__" and value must be the sequence of strings
class _PrivateAttrDict(TypedDict):
    __private_attrs__: Sequence[str]

_T = TypeVar('T')

class _PrivateWrap(Generic[_T]):
    @property
    def result(self) -> _T:
        "the final result of decorating"
        ...

    @property
    def funcs(self) -> tuple[FunctionType]:
        "the original functions"
        ...

    def __getattr__(self, name: str) -> Any:
        return getattr(self.result, name)

_TVar = TypeVar('TVar')

class PrivateWrapProxy:
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
                private_func: Callable[[int, str], str]|None = None) -> PrivateAttrType: ...

class PrivateAttrBase(metaclass=PrivateAttrType):
    "The class to help to create private attribute. It does not have any special behavior."
    __slots__ = ()
    __private_attrs__ = ()


class _PrivateTemp:
    @property
    def name(self) -> str: ...
    @property
    def bases(self) -> tuple[type]: ...
    @property
    def attrs(self) -> dict[str, Any]: ...
    @property
    def kwds(self) -> dict[str, Any]: ...

def prepare(name: str, bases: tuple, attrs: _PrivateAttrDict, /, **kwds) -> _PrivateTemp:
    """
    function for custom metaclass to create private attributes class.

    def prepare(name: str, bases: tuple, attrs: dict, **kwds) -> tempobject:
        the function to prepare for creating private attributes class. It will return a temporary object which has the same information as the arguments.

    def postprocess(type: type, tmp: tempobject) -> None:
        the function to postprocess for creating private attributes class. The custom metaclass can call this

    def register_metaclass(metaclass: type) -> None:
        the function to register custom metaclass. The custom metaclass must call this function to register itself before creating any private attributes class,
        otherwise the private attributes class created by this custom metaclass will not work.

    All usage of this module should be like:
    ```
    from abc import ABCMeta
    import private_attribute

    class PrivateAbcMeta(ABCMeta):
        def __new__(cls, *args, **kwargs):
            temp = private_attribute.prepare(*args, **kwargs)
            typ = super().__new__(cls, temp.name, temp.bases, temp.attrs, **temp.kwds)
            private_attribute.postprocess(typ, temp)
            return typ

    private_attribute.register_metaclass(PrivateAbcMeta)
    ```
    """
    ...

def postprocess(typ: type, temp: _PrivateTemp, /) -> None:
    """
    function for custom metaclass to create private attributes class.

    def prepare(name: str, bases: tuple, attrs: dict, **kwds) -> tempobject:
        the function to prepare for creating private attributes class. It will return a temporary object which has the same information as the arguments.

    def postprocess(type: type, tmp: tempobject) -> None:
        the function to postprocess for creating private attributes class. The custom metaclass can call this

    def register_metaclass(metaclass: type) -> None:
        the function to register custom metaclass. The custom metaclass must call this function to register itself before creating any private attributes class,
        otherwise the private attributes class created by this custom metaclass will not work.

    All usage of this module should be like:
    ```
    from abc import ABCMeta
    import private_attribute

    class PrivateAbcMeta(ABCMeta):
        def __new__(cls, *args, **kwargs):
            temp = private_attribute.prepare(*args, **kwargs)
            typ = super().__new__(cls, temp.name, temp.bases, temp.attrs, **temp.kwds)
            private_attribute.postprocess(typ, temp)
            return typ

    private_attribute.register_metaclass(PrivateAbcMeta)
    ```
    """
    ...
def register_metaclass(typ: type, /) -> None:
    """
    function for custom metaclass to create private attributes class.

    def prepare(name: str, bases: tuple, attrs: dict, **kwds) -> tempobject:
        the function to prepare for creating private attributes class. It will return a temporary object which has the same information as the arguments.

    def postprocess(type: type, tmp: tempobject) -> None:
        the function to postprocess for creating private attributes class. The custom metaclass can call this

    def register_metaclass(metaclass: type) -> None:
        the function to register custom metaclass. The custom metaclass must call this function to register itself before creating any private attributes class,
        otherwise the private attributes class created by this custom metaclass will not work.

    All usage of this module should be like:
    ```
    from abc import ABCMeta
    import private_attribute

    class PrivateAbcMeta(ABCMeta):
        def __new__(cls, *args, **kwargs):
            temp = private_attribute.prepare(*args, **kwargs)
            typ = super().__new__(cls, temp.name, temp.bases, temp.attrs, **temp.kwds)
            private_attribute.postprocess(typ, temp)
            return typ

    private_attribute.register_metaclass(PrivateAbcMeta)
    ```
    """
    ...
def ensure_type(typ: type, /) -> None:
    """function for custom metaclass to ensure the type is a private attributes class.
    def ensure_type(type: type) -> None:
        the function to ensure the type is a private attributes class `tp_getattro`, `tp_setattro` and `tp_finalizer`.
    """
    ...
def ensure_metaclass(typ: type, /) -> None:
    """function for custom metaclass to ensure the metaclass is working.
    def ensure_metaclass(metaclass: type) -> None:
        the function to ensure the metaclass `tp_getattro`, `tp_setattro` and `tp_finalizer`.
    """
    ...
