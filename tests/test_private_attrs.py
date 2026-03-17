import private_attribute
import unittest

class BaseClass(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_a", "_b"]
    __slots__ = ["c", "d"]

    def __init__(self, a, b, c, d):
        self._a = a
        self._b = b
        self.c = c
        self.d = d

    @property
    def a(self):
        return self._a

    @property
    def b(self):
        return self._b

    def public_has_a(self):
        return hasattr(self, "_a")

    def public_has_b(self):
        return hasattr(self, "_b")

class TestPrivateAttributes(unittest.TestCase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.base_obj = BaseClass(1, 2, 3, 4)

    def test_private_attrs(self):
        self.assertTrue(self.base_obj.public_has_a())
        self.assertTrue(self.base_obj.public_has_b())
        self.assertEqual(self.base_obj.a, 1)
        self.assertEqual(self.base_obj.b, 2)
        self.assertEqual(self.base_obj.c, 3)
        self.assertEqual(self.base_obj.d, 4)
        self.assertFalse(hasattr(self.base_obj, "_a"))
        self.assertFalse(hasattr(self.base_obj, "_b"))
        with self.assertRaises(AttributeError):
            self.base_obj._a
        with self.assertRaises(AttributeError):
            self.base_obj._b
        self.assertTrue(isinstance(self.base_obj.__private_attrs__, tuple))
        for i in self.base_obj.__private_attrs__:
            self.assertTrue(isinstance(i, tuple))

if __name__ == "__main__":
    unittest.main()
