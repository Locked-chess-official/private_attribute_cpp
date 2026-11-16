"""Finalizers for classes created by a private class used as a metaclass.

A private class can itself be a metaclass. Its ``__del__`` used to be the same
function as the registered-metaclass finalizer, and that path re-entered
metaclass ``__getattribute__`` after shutdown had already dropped the metaclass
registry. CPython then printed an ignored TypeError:

    The metaclass is not a register type or a subclass of the PrivateAttrType!
"""
import gc
import subprocess
import sys
import textwrap
import unittest

import private_attribute


_SCRIPT = textwrap.dedent(
    """
    import gc
    import private_attribute

    class PrivateMeta(type):
        __new__ = private_attribute.create_new()

    class MetaParent(type, metaclass=PrivateMeta):
        __private_attrs__ = ["_v"]

        def __init__(cls, name, bases, ns, **kw):
            super().__init__(name, bases, ns)
            cls._v = 1

        def read(cls):
            return cls._v

    class MetaChild(MetaParent):
        __private_attrs__ = []

    class Subject(metaclass=MetaParent):
        pass

    class SubjectChild(metaclass=MetaChild):
        pass

    if Subject.read() != 1 or SubjectChild.read() != 1:
        raise SystemExit("private value was not visible to the metaclass")

    del Subject, SubjectChild, MetaChild, MetaParent, PrivateMeta
    gc.collect()
    """
)


class TestPrivateMetaclassDel(unittest.TestCase):
    def test_process_exit_has_no_ignored_exception(self):
        proc = subprocess.run(
            [sys.executable, "-c", _SCRIPT],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn("Exception ignored", proc.stderr)
        self.assertNotIn("not a register type", proc.stderr)
        self.assertNotIn("Traceback", proc.stderr)

    def test_in_process_collect_records_no_unraisable(self):
        captured = []

        def hook(unraisable):
            captured.append(unraisable)

        old = sys.unraisablehook
        sys.unraisablehook = hook
        try:
            class PrivateMeta(type):
                __new__ = private_attribute.create_new()

            class MetaParent(type, metaclass=PrivateMeta):
                __private_attrs__ = ["_v"]

                def __init__(cls, name, bases, ns, **kw):
                    super().__init__(name, bases, ns)
                    cls._v = "ok"

                def read(cls):
                    return cls._v

            class Subject(metaclass=MetaParent):
                pass

            self.assertEqual(Subject.read(), "ok")
            del Subject
            gc.collect()
        finally:
            sys.unraisablehook = old
        self.assertEqual(captured, [])

    def test_ordinary_private_instance_del_still_runs(self):
        seen = []

        class C(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_x"]

            def __init__(self):
                self._x = 1

            @private_attribute.create_slot("__del__")
            def __del__(self):
                seen.append(self._x)

        obj = C()
        del obj
        gc.collect()
        self.assertEqual(seen, [1])


if __name__ == "__main__":
    unittest.main()
