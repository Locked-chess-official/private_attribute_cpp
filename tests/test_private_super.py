"""PrivateSuper / PrivateClassSuper (cross-class class-level private access).

PrivateSuper(A, B, C, obj).name reads B's class-level private attribute that was
defined for C (stored in all_type_subclass_attr[B][C]) and calls its
__get__(obj, A).

PrivateClassSuper(A, B, C, cls).name is the classmethod flavour: it resolves the
same storage but calls __get__(None, cls).

These are NOT for instance attribute cross-type access.
"""
import unittest

import private_attribute


class Grand(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_g"]
    _g = "grand"

    @classmethod
    def set_g(cls, v):
        cls._g = v

    @classmethod
    def get_g(cls):
        return cls._g


class Mid(Grand):
    __private_attrs__ = ["_m"]
    _m = "mid"


class Leaf(Mid, use_parents=True):
    __private_attrs__ = []

    def read_grand_shadow(self):
        # A=Leaf, B=Grand, C=Mid (subclass of B, parent of A)
        return private_attribute.PrivateSuper(Leaf, Grand, Mid, self)._g

    @classmethod
    def read_grand_shadow_cls(cls):
        return private_attribute.PrivateClassSuper(Leaf, Grand, Mid, cls)._g


class TestPrivateSuper(unittest.TestCase):
    def test_read_parent_shadow_for_intermediate(self):
        # Grand's code operating on Mid writes the shadow
        # all_type_subclass_attr[Grand][Mid].
        Mid.set_g("written-on-mid")
        self.assertEqual(Leaf().read_grand_shadow(), "written-on-mid")
        self.assertEqual(Leaf.read_grand_shadow_cls(), "written-on-mid")

    def test_c_can_be_b(self):
        # C == B -> reads B's own class body storage.
        class Child(Grand, use_parents=True):
            __private_attrs__ = []
            def read(self):
                return private_attribute.PrivateSuper(Child, Grand, Grand, self)._g

        self.assertEqual(Child().read(), "grand")

    def test_c_can_be_a(self):
        class Child(Grand, use_parents=True):
            __private_attrs__ = []
            _g = 5  # Child's own shadow for its own storage
            def read(self):
                # A=Child, B=Grand, C=Child (== A): Grand's shadow for Child.
                return private_attribute.PrivateSuper(Child, Grand, Child, self)._g
            @classmethod
            def write_parent_shadow(cls, v):
                # parent code (inherited from Grand) operating on Child
                Grand.set_g.__func__(cls, v)

        Child.write_parent_shadow(42)
        self.assertEqual(Child().read(), 42)
        self.assertEqual(Grand.get_g(), "grand")

    def test_descriptor_get_is_invoked(self):
        class Descr:
            def __get__(self, inst, owner):
                return ("obj", inst is not None, owner.__name__)
            def __set__(self, inst, v):
                pass

        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_d"]
            _d = Descr()
            @classmethod
            def set_d(cls, v):
                cls._d = v

        class C(P, use_parents=True):
            __private_attrs__ = []
            def read(self):
                return private_attribute.PrivateSuper(C, P, C, self)._d
            @classmethod
            def read_cls(cls):
                return private_attribute.PrivateClassSuper(C, P, C, cls)._d

        C.set_d(Descr())
        o = C()
        self.assertEqual(o.read(), ("obj", True, "C"))
        self.assertEqual(C.read_cls(), ("obj", False, "C"))

    def test_plain_value_returned_directly(self):
        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_n"]
            _n = 7
            @classmethod
            def set_n(cls, v):
                cls._n = v

        class C(P, use_parents=True):
            __private_attrs__ = []
            def read(self):
                return private_attribute.PrivateSuper(C, P, C, self)._n

        C.set_n(11)
        self.assertEqual(C().read(), 11)


class TestValidation(unittest.TestCase):
    def setUp(self):
        self.obj = Leaf()

    def test_b_must_be_parent_of_a(self):
        # Mid is not a parent of Grand.
        with self.assertRaises(ValueError):
            private_attribute.PrivateSuper(Grand, Mid, Mid, Grand())
        # An unrelated private type is not a parent of Leaf.
        class Other(private_attribute.PrivateAttrBase):
            __private_attrs__ = []
        with self.assertRaises(ValueError):
            private_attribute.PrivateSuper(Leaf, Other, Other, self.obj)

    def test_a_b_c_must_be_private_types(self):
        with self.assertRaises(ValueError):
            private_attribute.PrivateSuper(int, Grand, Mid, self.obj)
        with self.assertRaises(ValueError):
            private_attribute.PrivateSuper(Leaf, int, Mid, self.obj)
        with self.assertRaises(ValueError):
            private_attribute.PrivateSuper(Leaf, Grand, int, self.obj)

    def test_c_must_be_b_a_or_between(self):
        class Sibling(Grand):
            __private_attrs__ = []

        # Sibling is a subclass of Grand (B) but NOT a parent of Leaf (A).
        with self.assertRaises(ValueError):
            private_attribute.PrivateSuper(Leaf, Grand, Sibling, self.obj)

    def test_obj_must_be_instance_of_a(self):
        with self.assertRaises(TypeError):
            private_attribute.PrivateSuper(Leaf, Grand, Mid, object())

    def test_cls_must_be_subclass_of_a(self):
        with self.assertRaises(TypeError):
            private_attribute.PrivateClassSuper(Leaf, Grand, Mid, Grand)


class TestOwnership(unittest.TestCase):
    def test_access_from_non_a_code_denied(self):
        ps = private_attribute.PrivateSuper(Leaf, Grand, Mid, Leaf())
        with self.assertRaises(AttributeError):
            # module-level code is not Leaf's code
            ps._g

    def test_missing_attribute(self):
        def read_missing():
            return private_attribute.PrivateSuper(Leaf, Grand, Mid, Leaf())._nope

        with self.assertRaises(AttributeError):
            read_missing()


if __name__ == "__main__":
    unittest.main()
