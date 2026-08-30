"""Regression test for __slots__ traversal in private-attribute types.

A private-attribute class whose __slots__ member participates in a reference
cycle must still be collected by the cyclic GC. The instance-level traverse
wrapper previously skipped the __slots__ storage entirely (it only visited the
instance __dict__ and AllData-stored private attributes), so a cycle formed
purely through __slots__ members leaked.
"""
import gc
import unittest
import weakref

import private_attribute
from private_attribute import PrivateAttrBase


def _collect():
    for _ in range(3):
        gc.collect()


class TestSlotsCycleCollect(unittest.TestCase):
    def test_slots_pair_cycle_collected(self):
        class A(PrivateAttrBase):
            __private_attrs__ = ("_x",)
            __slots__ = ("peer", "__weakref__")

            def __init__(self):
                self._x = 1
                self.peer = None

        a = A()
        b = A()
        a.peer = b
        b.peer = a
        wa, wb = weakref.ref(a), weakref.ref(b)
        del a, b
        _collect()
        self.assertIsNone(wa())
        self.assertIsNone(wb())

    def test_slots_self_cycle_collected(self):
        class A(PrivateAttrBase):
            __private_attrs__ = ("_x",)
            __slots__ = ("self_ref", "__weakref__")

            def __init__(self):
                self._x = 1
                self.self_ref = self

        a = A()
        w = weakref.ref(a)
        del a
        _collect()
        self.assertIsNone(w())


if __name__ == "__main__":
    unittest.main()
