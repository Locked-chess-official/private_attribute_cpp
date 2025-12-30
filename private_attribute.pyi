from typing import Any, TypeVar, Callable, TypedDict, Sequence

# define the dict that must have a key "__private_attrs__" and value must be the sequence of strings
class PrivateAttrDict(TypedDict):
    __private_attrs__: Sequence[str]

T = TypeVar('T')

class _PrivateWrap[T]:
    @property
    def result(self) -> T: ...

class PrivateWrapProxy:
    def __init__(self, decorator: Callable[[Any], T], orig: _PrivateWrap|None = None, /) -> None: ...
    def __call__(self, func: Any) -> _PrivateWrap[T]: ...

class PrivateAttrType(type):
    def __new__(cls, name: str, bases: tuple, attrs: PrivateAttrDict, private_func: Callable[[int, str], str]|None = None) -> PrivateAttrType: ...

class PrivateAttrBase(metaclass=PrivateAttrType):
    __slots__ = ()
    __private_attrs__ = ()
