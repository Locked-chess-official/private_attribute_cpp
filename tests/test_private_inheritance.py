import private_attribute
import unittest

class BaseClass(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_value"]
    __slots__ = []

    def __init__(self, value):
        super().__init__()
        self._value = value

class SubClass(BaseClass):
    __private_attrs__ = ["_other_value"]
    __slots__ = []

    def __init__(self, value, other_value):
        super().__init__(value)
        self._other_value = other_value

    @property
    def value(self):
        return self._value

    @property
    def other_value(self):
        return self._other_value


class TestPrivateInheritance(unittest.TestCase):
    def test_private_inheritance(self):
        obj = SubClass(1, 2)
        self.assertEqual(obj.value, 1)
        self.assertEqual(obj.other_value, 2)
        with self.assertRaises(AttributeError):
            obj._value
        with self.assertRaises(AttributeError):
            obj._other_value

if __name__ == "__main__":
    unittest.main()
