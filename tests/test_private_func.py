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

    def test_private_func_local_function_gc(self):
        # Regression: a class created with a LOCAL private_func must not leave
        # a dangling pointer in type_need_call after the function is collected.
        # Previously this crashed with a segfault in PrivateAttrType_finalize
        # during gc.collect().
        import gc

        def local_func(obj_id, attr):
            return f"local_{obj_id}_{attr}"

        class LocalParent(private_attribute.PrivateAttrBase, private_func=local_func):
            __private_attrs__ = ["_v"]

            def __init__(self):
                self._v = 1

            def get_v(self):
                return self._v

        class LocalChild(LocalParent):
            __private_attrs__ = []

        obj = LocalChild()
        self.assertEqual(obj.get_v(), 1)
        del obj
        # drop the local classes and the local function, then collect
        del LocalChild, LocalParent, local_func
        for _ in range(3):
            gc.collect()

if __name__ == "__main__":
    unittest.main()
