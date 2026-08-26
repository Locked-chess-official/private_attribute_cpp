"""attr_classify ownership rule (v2.1.3).

When a code object is owned by several classes on the MRO chain (e.g. a
subclass aliases a parent method with `f = g`, or copies a parent method's
bytecode with `f.__code__ = g.__code__`), attr_classify must attribute the
code to the base-most (declaring, parent) class. Otherwise the parent code's
private attribute accesses get split across two storages: the tampered method
writes the subclass's storage while the untampered parent method reads the
parent's storage, corrupting the parent's behavior.

Key invariant: parent methods inc() (tampered by the child) and peek() (not
tampered) must ALWAYS see the same storage on a child instance.
"""
import unittest

import private_attribute


class Parent(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_n"]
    _n = 0

    def inc(self):
        self._n += 1
        return self._n

    def peek(self):
        return self._n


class TestAliasCodeOwnership(unittest.TestCase):
    """`f = g` in the class body registers the same function object with both
    classes; the aliased and the untampered parent methods must stay
    consistent on a child instance."""

    def test_alias_method_sees_same_storage_as_untampered(self):
        class AliasChild(Parent):
            __private_attrs__ = ["_n"]
            _n = 100
            f = Parent.inc          # same function object in both class dicts

        c = AliasChild()
        v1 = c.f()                  # parent code, double-owned
        v2 = c.peek()               # parent code, single-owned
        self.assertEqual(v1, v2)
        c.f()
        self.assertEqual(c.peek(), v2 + 1)
        self.assertEqual(Parent().peek(), 0)

    def test_alias_without_shadow_works(self):
        class AliasChild2(Parent):
            __private_attrs__ = []
            f = Parent.inc

        c = AliasChild2()
        v1 = c.f()
        self.assertEqual(v1, c.peek())


class TestCodeSwapOwnership(unittest.TestCase):
    """`f.__code__ = g.__code__` inside the class body; the swapped method
    must behave exactly like the parent's method (same storage)."""

    def test_code_swap_sees_same_storage_as_untampered(self):
        class CodeChild(Parent):
            __private_attrs__ = ["_n"]
            _n = 100

            def f(self):
                return None

            f.__code__ = Parent.inc.__code__

        c = CodeChild()
        v1 = c.f()
        v2 = c.peek()
        self.assertEqual(v1, v2)
        c.f()
        self.assertEqual(c.peek(), v2 + 1)


class TestFallbackSemantics(unittest.TestCase):
    """Per-subject fallback is preserved: an unwritten read resolves to the
    subject's own class-level default, consistently for both methods."""

    def test_unwritten_read_falls_back_per_subject(self):
        class FreshChild(Parent):
            __private_attrs__ = ["_n"]
            _n = 100
            f = Parent.inc

        c = FreshChild()
        self.assertEqual(c.peek(), 100)
        v1 = c.f()
        self.assertEqual(v1, c.peek())


class TestNormalUsageRegression(unittest.TestCase):
    """Without tampering, nothing changes: child's own code uses child
    storage, parent's code keeps its per-subject view."""

    def test_child_own_code_uses_child_storage(self):
        class NormChild(Parent):
            __private_attrs__ = ["_n"]
            _n = 100

            def own_inc(self):
                self._n += 1
                return self._n

        c = NormChild()
        self.assertEqual(c.own_inc(), 101)
        self.assertEqual(c.peek(), 100)
        self.assertEqual(Parent().peek(), 0)


class TestIsolationIntact(unittest.TestCase):
    """The v2.1.0 isolation rule still holds: the subclass's own code must
    not touch a parent's private attribute."""

    def test_subclass_own_code_denied(self):
        class IsoParent(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_secret"]
            _secret = 1

        class IsoChild(IsoParent):
            __private_attrs__ = []

            def read_secret(self):
                return self._secret

        with self.assertRaises(AttributeError):
            IsoChild().read_secret()


if __name__ == "__main__":
    unittest.main()
