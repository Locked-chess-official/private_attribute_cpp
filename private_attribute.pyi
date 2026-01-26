from typing import Any, TypeVar, Callable, TypedDict, Sequence, Generic
from types import FunctionType

# define the dict that must have a key "__private_attrs__" and value must be the sequence of strings
class PrivateAttrDict(TypedDict):
    __private_attrs__: Sequence[str]

T = TypeVar('T')

class _PrivateWrap(Generic[T]):
    @property
    def result(self) -> T: ...

    @property
    def funcs(self) -> tuple[FunctionType]: ...

    def __getattr__(self, name: str) -> Any:
        return getattr(self.result, name)

TVar = TypeVar('TVar')

class PrivateWrapProxy:
    def __init__(self, decorator: Callable[[TVar], T], orig: _PrivateWrap[Any]|None = None, /) -> None: ...
    def __call__(self, func: TVar, /) -> _PrivateWrap[T]: ...

class PrivateAttrType(type):
    def __new__(cls, name: str, bases: tuple,
                attrs: PrivateAttrDict, /,
                private_func: Callable[[int, str], str]|None = None) -> PrivateAttrType: ...

class PrivateAttrBase(metaclass=PrivateAttrType):
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

def prepare(name: str, bases: tuple, attrs: PrivateAttrDict, /, **kwds) -> _PrivateTemp: ...
def postprocess(typ: type, temp: _PrivateTemp, /) -> None: ...
def register_metaclass(typ: type, /) -> None: ...
def ensure_type(typ: type, /) -> None: ...
def ensure_metaclass(typ: type, /) -> None: ...
