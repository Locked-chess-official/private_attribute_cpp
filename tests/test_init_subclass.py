import private_attribute
import unittest


class TestInitSubclassWrite(unittest.TestCase):
    """A parent may write a private attribute into a subclass (e.g. via
    __init_subclass__); later, the parent's own code reading the subclass (or
    an instance of it) must see the parent-stored value, while the subclass's
    own same-name private storage stays independent."""

    def test_init_subclass_write_visible_to_parent_code(self):
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]
            _value = "parent_own"

            def __init_subclass__(cls, **kw):
                super().__init_subclass__(**kw)
                cls._value = "from_parent"
                cls.setup()

            @classmethod
            def get_cls_value(cls):
                return cls._value

            def get_value(self):
                return self._value

            @classmethod
            def setup(cls): ...

        class Child(Parent):
            __private_attrs__ = ["_value", "_other_value"]

            def __init__(self):
                super().__init__()
                self._value = "child_own"

            def get_own(self):
                return self._value

            @classmethod
            def setup(cls):
                cls._other_value = 2

            @classmethod
            def get_other_value(cls):
                return cls._other_value

        # parent code reading the subclass class / instance sees the
        # parent-stored value
        self.assertEqual(Child.get_cls_value(), "from_parent")
        self.assertEqual(Child().get_value(), "from_parent")
        # the subclass's own code reads/writes its own independent storage
        self.assertEqual(Child().get_own(), "child_own")
        # the parent's own storage is untouched
        self.assertEqual(Parent().get_value(), "parent_own")
        # parent successfully called Child.setup()
        self.assertTrue(Child.get_other_value(), 2)
        # private attribute stays hidden from outside
        with self.assertRaises(AttributeError):
            Child()._value
        with self.assertRaises(AttributeError):
            Child._value

    def test_parent_writes_child_after_creation(self):
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]
            _value = "parent_own"

            @classmethod
            def write_child(cls, child_cls, v):
                child_cls._value = v

            def get_value(self):
                return self._value

            @classmethod
            def get_cls_value(cls):
                return cls._value

        class Child(Parent):
            __private_attrs__ = ["_value"]

            def __init__(self):
                super().__init__()
                self._value = "child_own"

            def get_own(self):
                return self._value

        Parent.write_child(Child, "from_parent")
        self.assertEqual(Child.get_cls_value(), "from_parent")
        self.assertEqual(Child().get_value(), "from_parent")
        self.assertEqual(Child().get_own(), "child_own")
        self.assertEqual(Parent().get_value(), "parent_own")


if __name__ == "__main__":
    unittest.main()
