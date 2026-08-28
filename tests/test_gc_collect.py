"""GC regression tests for the metaclass type_traverse/type_clear fix.

Validates that class objects created by the private-attribute machinery are
actually reclaimed by the cyclic GC (previously they leaked because the
metaclass traverse resolved to the instance-level delegating defaults instead
of CPython's real type_traverse/type_clear).

Run: PYTHONPATH=. python test_gc_collect.py
"""
import gc
import sys
import unittest
import weakref

import private_attribute
from private_attribute import PrivateAttrBase


def make_class(i, name="C"):
    class C(PrivateAttrBase):
        __private_attrs__ = ("_x",)
        def __init__(self):
            self._x = i
        def get(self):
            return self._x
    C.__name__ = name
    return C


def alive_after_del_collect(maker, n):
    """Create n classes, drop them, collect; return how many are still alive."""
    refs = []
    for i in range(n):
        C = maker(i, f"Probe{i}")
        refs.append(weakref.ref(C))
        del C
    for _ in range(3):
        gc.collect()
    return sum(1 for w in refs if w() is not None)


class TestClassCollect(unittest.TestCase):
    def test_empty_class_collected(self):
        def maker(i, name):
            class E(PrivateAttrBase):
                pass
            E.__name__ = name
            return E
        self.assertEqual(alive_after_del_collect(maker, 20), 0)

    def test_class_with_private_attrs_collected(self):
        self.assertEqual(alive_after_del_collect(make_class, 20), 0)

    def test_class_with_private_value_freed(self):
        class Holder:
            pass
        refs = []
        for i in range(20):
            h = Holder()
            class H(PrivateAttrBase):
                __private_attrs__ = ("_cv",)
                _cv = h
            refs.append(weakref.ref(h))
            del h, H
        for _ in range(3):
            gc.collect()
        self.assertEqual(sum(1 for w in refs if w() is not None), 0)

    def test_mutual_class_cycle_collected(self):
        def cyc():
            class A(PrivateAttrBase):
                __private_attrs__ = ("_ref",)
                _ref = None
                def set_ref(cls, v):
                    cls._ref = v
            class B(PrivateAttrBase):
                __private_attrs__ = ("_ref",)
                _ref = None
                def set_ref(cls, v):
                    cls._ref = v
            A.set_ref(A, B)
            B.set_ref(B, A)
            wa = weakref.ref(A)
            wb = weakref.ref(B)
            del A, B
            for _ in range(3):
                gc.collect()
            return wa() is not None, wb() is not None
        a, b = cyc()
        self.assertFalse(a)
        self.assertFalse(b)

    def test_self_referencing_class_collected(self):
        def cyc():
            class S(PrivateAttrBase):
                __private_attrs__ = ("_self_ref",)
                _self_ref = None
                def set_self(cls, v):
                    cls._self_ref = v
            S.set_self(S, S)
            w = weakref.ref(S)
            del S
            for _ in range(3):
                gc.collect()
            return w() is not None
        self.assertFalse(cyc())

    def test_nested_class_collected(self):
        def nested():
            class Outer(PrivateAttrBase):
                __private_attrs__ = ("_inner",)
                def make(self):
                    class Inner(PrivateAttrBase):
                        __private_attrs__ = ("_v",)
                        def __init__(self):
                            self._v = 1
                    self._inner = Inner
                    return Inner
            o = Outer()
            Inner = o.make()
            wi = weakref.ref(Inner)
            wo = weakref.ref(o)
            del o, Inner
            for _ in range(3):
                gc.collect()
            return wi() is not None, wo() is not None
        inner, outer = nested()
        self.assertFalse(inner)
        self.assertFalse(outer)

    def test_private_attr_semantics_still_work(self):
        class C(PrivateAttrBase):
            __private_attrs__ = ("_x",)
            def __init__(self):
                self._x = 7
            def get(self):
                return self._x
        c = C()
        self.assertEqual(c.get(), 7)
        with self.assertRaises(AttributeError):
            c._x


if __name__ == "__main__":
    unittest.main()
