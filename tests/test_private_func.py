import private_attribute
import unittest

def my_func(obj_id, attr):
    return f"Object ID: {obj_id}, String: {attr}"

class MyClass(private_attribute.PrivateAttrBase, private_func=my_func):
    __private_attrs__ = ["_secret"]
    __slots__ = ["public_attr"]

    def __init__(self, secret, public_attr):
        self._secret = secret
        self.public_attr = public_attr

    def get_secret(self):
        return self._secret

class TestPrivateFunc(unittest.TestCase):
    def test_private_func(self):
        obj = MyClass("12345", "public_value")
        self.assertEqual(obj.get_secret(), "12345")
        self.assertEqual(obj.public_attr, "public_value")
        with self.assertRaises(AttributeError):
            obj._secret

if __name__ == "__main__":
    unittest.main()
