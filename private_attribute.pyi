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


class private_temp:
    @property
    def name(self) -> str: ...
    @property
    def bases(self) -> tuple[type]: ...
    @property
    def attrs(self) -> PrivateAttrDict: ...
    @property
    def kwds(self) -> dict[str, Any]: ...

def prepare(name, bases, attrs, **kwds) -> private_temp: ...
def postprocess(typ: type, temp: private_temp) -> None: ...
def register_metaclass(typ: type) -> None: ...
