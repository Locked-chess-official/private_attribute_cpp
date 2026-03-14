import private_attribute
import unittest

class MyWrap:
    def __init__(self, func):
        self.func1 = func
        self.index = 1

    def add(self, func):
        self.index += 1
        setattr(self, f"func{self.index}", func)
        return self

    def get(self, index, instance, owner):
        func =  getattr(self, f"func{index}")
        return func.__get__(instance, owner)

class MyClass(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_secret1", "_secret2", "_secret3"]
    def __init__(self, secret1, secret2, secret3):
        self._secret1 = secret1
        self._secret2 = secret2
        self._secret3 = secret3

    @private_attribute.PrivateWrapProxy(MyWrap)
    def get_secret(self):
        return self._secret1

    @private_attribute.PrivateWrapProxy(get_secret.result.add, get_secret)
    def get_secret(self):
        return self._secret2

    @private_attribute.PrivateWrapProxy(get_secret.result.add, get_secret)
    def get_secret(self):
        return self._secret3

class TestPrivateWrap(unittest.TestCase):
    def test_private_wrap(self):
        a = MyClass(1, 2, 3)
        self.assertEqual(a.get_secret.get(1, a, MyClass)(), 1)
        self.assertEqual(a.get_secret.get(2, a, MyClass)(), 2)
        self.assertEqual(a.get_secret.get(3, a, MyClass)(), 3)
        with self.assertRaises(AttributeError):
            a._secret1
        with self.assertRaises(AttributeError):
            a._secret2
        with self.assertRaises(AttributeError):
            a._secret3

if __name__ == "__main__":
    unittest.main()