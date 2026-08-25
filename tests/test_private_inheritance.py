import private_attribute
import unittest

class BaseClass(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_value"]
    __slots__ = []

    def __init__(self, value):
        super().__init__()
        self._value = value

    @property
    def value(self):
        return self._value


class SubClass(BaseClass):
    __private_attrs__ = ["_other_value"]
    __slots__ = []

    def __init__(self, value, other_value):
        super().__init__(value)
        self._other_value = other_value

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

    def test_subclass_cannot_access_parent_private(self):
        # 2.1.0: code of a subclass must not touch a parent's private attribute.
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_secret"]

            def __init__(self):
                self._secret = 1

        class Child(Parent):
            __private_attrs__ = []

            def read_parent_secret(self):
                return self._secret

            def write_parent_secret(self):
                self._secret = 2

        obj = Child()
        with self.assertRaises(AttributeError):
            obj.read_parent_secret()
        with self.assertRaises(AttributeError):
            obj.write_parent_secret()

    def test_same_name_instance_attrs_stored_separately(self):
        # 2.1.0: subclass and parent may both declare the same private name;
        # each class's code accesses its own storage.
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]

            def __init__(self, value):
                self._value = value

            def get_parent_value(self):
                return self._value

        class Child(Parent):
            __private_attrs__ = ["_value"]

            def __init__(self, value):
                super().__init__(value)      # writes Parent's _value
                self._value = value * 10     # writes Child's _value

            def get_child_value(self):
                return self._value

        obj = Child(3)
        self.assertEqual(obj.get_parent_value(), 3)
        self.assertEqual(obj.get_child_value(), 30)
        with self.assertRaises(AttributeError):
            obj._value

    def test_same_name_class_attrs_stored_separately(self):
        # 2.1.0: a class-body attribute shadowing a parent's private name
        # becomes this class's own private attribute.
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]
            _value = 10

            def get_parent_value(self):
                return self._value

        class Child(Parent):
            __private_attrs__ = []
            _value = 20

            def get_child_value(self):
                return self._value

        obj = Child()
        self.assertEqual(obj.get_parent_value(), 10)
        self.assertEqual(obj.get_child_value(), 20)
        with self.assertRaises(AttributeError):
            obj._value
        with self.assertRaises(AttributeError):
            Child._value

    def test_parent_code_still_works_on_subclass_instance(self):
        # Parent's own methods keep working on subclass instances.
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]

            def __init__(self, value):
                self._value = value

            def get_value(self):
                return self._value

        class Child(Parent):
            __private_attrs__ = ["_other"]

            def __init__(self, value, other):
                super().__init__(value)
                self._other = other

            def get_other(self):
                return self._other

        obj = Child(5, 6)
        self.assertEqual(obj.get_value(), 5)
        self.assertEqual(obj.get_other(), 6)

    def test_multi_level_inheritance(self):
        # A private attribute is only reachable from the code of the class
        # that declares it - not from any of its subclasses.
        class GrandParent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_g"]

            def __init__(self):
                self._g = 1

            def get_g(self):
                return self._g

        class Parent(GrandParent):
            __private_attrs__ = ["_p"]

            def __init__(self):
                super().__init__()
                self._p = 2

            def get_p(self):
                return self._p

            def try_grandparent_private(self):
                return self._g

        class Child(Parent):
            __private_attrs__ = []

            def try_parent_private(self):
                return self._p

            def try_grandparent_private(self):
                return self._g

        obj = Child()
        self.assertEqual(obj.get_g(), 1)
        self.assertEqual(obj.get_p(), 2)
        with self.assertRaises(AttributeError):
            obj.try_parent_private()
        with self.assertRaises(AttributeError):
            obj.try_grandparent_private()
        with self.assertRaises(AttributeError):
            Parent().try_grandparent_private()

    def test_multiple_inheritance(self):
        class Left(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]

            def __init__(self):
                self._value = "left"

            def get_value(self):
                return self._value

        class Right(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]

            def __init__(self):
                self._value = "right"

            def get_value(self):
                return self._value

        class Both(Left, Right):
            __private_attrs__ = ["_value"]

            def __init__(self):
                Left.__init__(self)
                Right.__init__(self)
                self._value = "both"

            def get_value(self):
                return self._value

        obj = Both()
        self.assertEqual(Left.get_value(obj), "left")
        self.assertEqual(Right.get_value(obj), "right")
        self.assertEqual(obj.get_value(), "both")
        with self.assertRaises(AttributeError):
            obj._value

    def test_private_func_with_inheritance(self):
        def gen(obj_id, attr_name):
            return f"<{attr_name}@{obj_id}>"

        class Parent(private_attribute.PrivateAttrBase, private_func=gen):
            __private_attrs__ = ["_value"]

            def __init__(self, value):
                self._value = value

            def get_value(self):
                return self._value

        class Child(Parent):
            __private_attrs__ = ["_value"]

            def __init__(self, value):
                super().__init__(value)
                self._value = value * 2

            def get_value(self):
                return self._value

        obj = Child(4)
        self.assertEqual(Parent.get_value(obj), 4)
        self.assertEqual(obj.get_value(), 8)

    def test_class_level_del_from_own_code(self):
        class Parent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_value"]
            _value = 1

            @classmethod
            def del_value(cls):
                del cls._value

            @classmethod
            def set_value(cls, v):
                cls._value = v

            @classmethod
            def get_value(cls):
                return cls._value

        self.assertEqual(Parent.get_value(), 1)
        Parent.del_value()
        with self.assertRaises(AttributeError):
            Parent.get_value()
        # setting again works after deletion
        Parent.set_value(99)
        self.assertEqual(Parent.get_value(), 99)

if __name__ == "__main__":
    unittest.main()
