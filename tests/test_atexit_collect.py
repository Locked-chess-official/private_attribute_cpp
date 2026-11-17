"""Exit cleanup collects cyclic garbage before dropping private storage.

CPython runs PyGC_Collect only after atexit. Private-attribute tables are
cleared in an atexit hook, and after that a private __del__ callback is not
invoked. Collecting first lets a cycle's __del__ read private attributes.
gc.collect() still runs when gc.disable() is in effect; PyGC_Collect() would
not.
"""
import subprocess
import sys
import textwrap
import unittest


def _run(script):
    return subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True,
        text=True,
        check=False,
    )


_CYCLE = textwrap.dedent(
    """
    import sys
    import private_attribute

    class C(private_attribute.PrivateAttrBase):
        __private_attrs__ = ["_x"]

        def __init__(self, n):
            self._x = n

        @private_attribute.create_slot("__del__")
        def __del__(self):
            sys.stderr.write("CYCLE_DEL %s\\n" % self._x)
            sys.stderr.flush()

    a = C(1)
    b = C(2)
    a.other = b
    b.other = a
    del a, b
    """
)

_DISABLED = "import gc\ngc.disable()\n" + _CYCLE

_TABLE_ONLY = textwrap.dedent(
    """
    import sys
    import private_attribute

    class Only:
        def __del__(self):
            sys.stderr.write("ONLY_PRIVATE_DEL\\n")
            sys.stderr.flush()

    class Holder(private_attribute.PrivateAttrBase):
        __private_attrs__ = ["_box"]

        def __init__(self):
            self._box = Only()

    h = Holder()
    """
)


class TestAtexitCollect(unittest.TestCase):
    def test_cyclic_del_reads_private_attribute(self):
        proc = _run(_CYCLE)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("CYCLE_DEL 1", proc.stderr)
        self.assertIn("CYCLE_DEL 2", proc.stderr)
        self.assertNotIn("Exception ignored", proc.stderr)
        self.assertNotIn("Traceback", proc.stderr)

    def test_cyclic_del_runs_when_gc_disabled(self):
        proc = _run(_DISABLED)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("CYCLE_DEL 1", proc.stderr)
        self.assertIn("CYCLE_DEL 2", proc.stderr)
        self.assertNotIn("Exception ignored", proc.stderr)

    def test_value_held_only_by_private_storage_is_still_finalized(self):
        # The holder stays reachable from module globals through atexit, so
        # the collect cannot see the value. Table cleanup must still drop it.
        proc = _run(_TABLE_ONLY)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("ONLY_PRIVATE_DEL", proc.stderr)
        self.assertNotIn("Exception ignored", proc.stderr)


if __name__ == "__main__":
    unittest.main()
