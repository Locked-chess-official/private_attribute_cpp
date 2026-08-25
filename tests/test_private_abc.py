from abc import ABCMeta, abstractmethod, ABC
import private_attribute
import unittest


class PrivateAbcMeta(ABCMeta):
    def __new__(cls, *args, **kwargs):
        temp = private_attribute.prepare(*args, **kwargs)
        typ = super().__new__(cls, temp.name, temp.bases, temp.attrs, **temp.kwds)
        private_attribute.postprocess(typ, temp)
        return typ


private_attribute.register_metaclass(PrivateAbcMeta)

class MyClass(ABC, metaclass=PrivateAbcMeta):
    __private_attrs__ = ["_secret1"]
    _secret1 = 10

    @classmethod
    def get_secret1(cls):
        return cls._secret1

    @abstractmethod
    def public_method(self):
        pass

class ConcreteClass(MyClass):
    # 2.1.0: a subclass can no longer access a parent's private attribute.
    # If it needs the same name, it declares its own -> stored separately.
    __private_attrs__ = ["_secret1", "_secret2"]

    def __init__(self, secret1, secret2):
        self._secret1 = secret1
        self._secret2 = secret2

    def public_method(self):
        return self._secret1, self._secret2


class TestPrivateAbc(unittest.TestCase):
    def test_private_abc(self):
        c = ConcreteClass("secret1", "secret2")
        self.assertEqual(c.public_method(), ("secret1", "secret2"))
        with self.assertRaises(AttributeError):
            c._secret1
        with self.assertRaises(AttributeError):
            c._secret2
        with self.assertRaises(TypeError):
            MyClass()
        with self.assertRaises(AttributeError):
            MyClass._secret1
        self.assertEqual(MyClass.get_secret1(), 10)
        # get_secret1 is MyClass's own code, so it reads MyClass's private
        # attribute even when called through the subclass.
        self.assertEqual(ConcreteClass.get_secret1(), 10)

if __name__ == '__main__':
    unittest.main()
