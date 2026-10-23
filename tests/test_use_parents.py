import private_attribute
import unittest


class TestUseParents(unittest.TestCase):
    def test_forbidden_parent_private_in_namespace(self):
        # 2.2.0: a subclass must not write a parent-private attribute into its
        # own namespace unless it is declared in __private_attrs__ or
        # use_parents=True is set.
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_secret"]

            def __init__(self):
                self._secret = 1

        with self.assertRaises(TypeError):
            class Child(Parent):
                __private_attrs__ = []
                _secret = 999

    def test_use_parents_instance_storage(self):
        # 2.2.0: use_parents=True lets the subclass code read/write the
        # parent's private name, but the value lives in the subclass's own
        # storage; the parent's own storage is untouched.
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_secret"]

            def __init__(self):
                self._secret = 1

            def get_secret(self):
                return self._secret

        class Child(Parent, use_parents=True):
            __private_attrs__ = []

            def __init__(self):
                super().__init__()
                self._secret = 99

            def read_own(self):
                return self._secret

            def write_own(self, v):
                self._secret = v

        obj = Child()
        # parent's code sees the parent's own value
        self.assertEqual(obj.get_secret(), 1)
        # child's code sees its own value
        self.assertEqual(obj.read_own(), 99)
        # child writes into its own storage only
        obj.write_own(7)
        self.assertEqual(obj.read_own(), 7)
        self.assertEqual(obj.get_secret(), 1)
        with self.assertRaises(AttributeError):
            obj._secret

    def test_use_parents_class_body_value(self):
        # 2.2.0: with use_parents=True a subclass may define a parent-private
        # name in its own body; it is stored in the subclass's own storage.
        # Parent code always reads the parent's own storage.
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]
            _value = 10

            def get_value(self):
                return self._value

        class Child(Parent, use_parents=True):
            __private_attrs__ = []
            _value = 20

            def get_own(self):
                return self._value

        self.assertEqual(Parent().get_value(), 10)
        obj = Child()
        self.assertEqual(obj.get_value(), 10)
        self.assertEqual(obj.get_own(), 20)
        with self.assertRaises(AttributeError):
            obj._value

    def test_use_parents_not_inherited_by_grandchild(self):
        # 2.2.0: use_parents merges the ancestor sets only into the class that
        # opts in; a grandchild without use_parents neither sees nor may
        # define the grandparent's private name.
        class GrandParent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_g"]

        class Parent(GrandParent, use_parents=True):
            __private_attrs__ = []
            _g = 1

        # Child (no use_parents) cannot define the grandparent-private name.
        with self.assertRaises(TypeError):
            class Child(Parent):
                __private_attrs__ = []
                _g = 2

        class Child2(Parent):
            __private_attrs__ = []

            def read_g(self):
                return self._g

        c = Child2()
        with self.assertRaises(AttributeError):
            c.read_g()

    def test_use_parents_false_is_default(self):
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_secret"]

        # default (no kwarg) behaves like use_parents=False
        with self.assertRaises(TypeError):
            class Child(Parent):
                __private_attrs__ = []
                _secret = 1

        # explicit False behaves the same
        with self.assertRaises(TypeError):
            class Child2(Parent, use_parents=False):
                __private_attrs__ = []
                _secret = 1


if __name__ == "__main__":
    unittest.main()
