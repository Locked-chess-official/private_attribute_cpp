import private_attribute
import unittest

class MyClass(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_secret"]
    _secret = 1

    @classmethod
    def get_secret(cls):
        return cls._secret

class TestClassPrivateAttrs(unittest.TestCase):
    def test_class_private_attrs(self):
        self.assertEqual(MyClass.get_secret(), 1)
        with self.assertRaises(AttributeError):
            MyClass._secret = 2
        a = MyClass()
        with self.assertRaises(AttributeError):
            a._secret = 2
        self.assertEqual(a.get_secret(), 1)

if __name__ == "__main__":
    unittest.main()
