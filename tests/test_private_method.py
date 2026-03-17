import private_attribute
import unittest

class PrivateMethodClass(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_secret_method", "_a"]
    __slots__ = ["public_attr"]

    def __init__(self, a):
        self._a = a
        self.public_attr = "public_value"

    def _secret_method(self):
        return self._a

    def public_method(self):
        return self._secret_method()


class TestPrivateMethod(unittest.TestCase):
    def test_private_method(self):
        obj = PrivateMethodClass(1)
        self.assertEqual(obj.public_method(), 1)
        with self.assertRaises(AttributeError):
            obj._secret_method()

        with self.assertRaises(AttributeError):
            obj._a

if __name__ == "__main__":
    unittest.main()
