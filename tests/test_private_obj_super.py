"""PrivateObjSuper (cross-class per-instance private access).

PrivateObjSuper(now_class, parent_class, obj).name reads the value parent_class
stored on obj. The running code must belong to now_class. The helper is
read-only and does not consult class-level storage (that is PrivateSuper).
"""
import gc
import unittest
import weakref

import private_attribute


class Parent(private_attribute.PrivateAttrBase):
    __private_attrs__ = ["_x", "_y"]
    public = "PUBLIC"

    def __init__(self, x):
        self._x = x

    def get_x(self):
        return self._x

    def set_x(self, value):
        self._x = value

    def del_x(self):
        del self._x


class Child(Parent):
    __private_attrs__ = ["_x"]

    def __init__(self, x, own):
        super().__init__(x)
        self._x = own

    def read_own(self):
        return self._x

    def read_parent(self):
        return private_attribute.PrivateObjSuper(Child, Parent, self)._x

    def read_missing(self):
        return private_attribute.PrivateObjSuper(Child, Parent, self)._y

    def read_public(self):
        return private_attribute.PrivateObjSuper(Child, Parent, self).public

    def read_nested(self):
        def inner():
            return private_attribute.PrivateObjSuper(Child, Parent, self)._x
        return inner()

    def read_lambda(self):
        return (lambda: private_attribute.PrivateObjSuper(Child, Parent, self)._x)()

    @classmethod
    def read_cm(cls, obj):
        return private_attribute.PrivateObjSuper(Child, Parent, obj)._x

    @staticmethod
    def read_sm(obj):
        return private_attribute.PrivateObjSuper(Child, Parent, obj)._x

    @property
    def via_prop(self):
        return private_attribute.PrivateObjSuper(Child, Parent, self)._x

    def try_set(self):
        proxy = private_attribute.PrivateObjSuper(Child, Parent, self)
        proxy._x = 5

    def try_del(self):
        proxy = private_attribute.PrivateObjSuper(Child, Parent, self)
        del proxy._x

    def flags(self):
        proxy = private_attribute.PrivateObjSuper(Child, Parent, self)
        return (
            hasattr(proxy, "_x"),
            hasattr(proxy, "_y"),
            hasattr(proxy, "public"),
            "_x" in dir(proxy),
        )

    def make(self):
        return private_attribute.PrivateObjSuper(Child, Parent, self)


class TestNormalUsage(unittest.TestCase):
    def test_reads_parent_storage_not_child(self):
        obj = Child(1, 99)
        self.assertEqual(obj.get_x(), 1)
        self.assertEqual(obj.read_own(), 99)
        self.assertEqual(obj.read_parent(), 1)
        # Ordinary access from outside the declaring class stays hidden.
        with self.assertRaises(AttributeError):
            obj._x

    def test_nested_code_classmethod_staticmethod_property(self):
        obj = Child(2, 3)
        self.assertEqual(obj.read_nested(), 2)
        self.assertEqual(obj.read_lambda(), 2)
        self.assertEqual(Child.read_cm(obj), 2)
        self.assertEqual(Child.read_sm(obj), 2)
        self.assertEqual(obj.via_prop, 2)

    def test_further_subclass_instance_is_accepted(self):
        class Grand(Child):
            __private_attrs__ = []

        obj = Grand(4, 5)
        # now_class is Child; a Grand instance is still an instance of Child.
        self.assertEqual(Child.read_cm(obj), 4)

    def test_instances_are_isolated(self):
        self.assertEqual(Child(1, 0).read_parent(), 1)
        self.assertEqual(Child(7, 0).read_parent(), 7)

    def test_parent_write_and_delete_are_visible(self):
        obj = Child(1, 2)
        obj.set_x(8)
        self.assertEqual(obj.read_parent(), 8)
        self.assertEqual(obj.read_own(), 2)
        obj.del_x()
        with self.assertRaises(AttributeError) as cm:
            obj.read_parent()
        self.assertIn("no attribute '_x'", str(cm.exception))

    def test_use_parents_keeps_storages_separate(self):
        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_s"]

            def __init__(self):
                self._s = "parent"

            def get(self):
                return self._s

        class C(P, use_parents=True):
            __private_attrs__ = []

            def __init__(self):
                super().__init__()
                self._s = "child"

            def own(self):
                return self._s

            def parent_value(self):
                return private_attribute.PrivateObjSuper(C, P, self)._s

        obj = C()
        self.assertEqual(obj.get(), "parent")
        self.assertEqual(obj.own(), "child")
        self.assertEqual(obj.parent_value(), "parent")

    def test_slots(self):
        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_s"]
            __slots__ = ()

            def __init__(self):
                self._s = "slotted"

        class C(P):
            __private_attrs__ = []
            __slots__ = ()

            def read(self):
                return private_attribute.PrivateObjSuper(C, P, self)._s

        self.assertEqual(C().read(), "slotted")

    def test_private_wrap_proxy_method(self):
        def ident(fn):
            return fn

        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_x"]

            def __init__(self):
                self._x = 8

        class C(P):
            __private_attrs__ = []

            @private_attribute.PrivateWrapProxy(ident)
            def read(self):
                return private_attribute.PrivateObjSuper(C, P, self)._x

        self.assertEqual(C().read(), 8)

    def test_exported_on_the_module(self):
        self.assertIn("PrivateObjSuper", private_attribute.__all__)
        self.assertIn("PrivateObjSuper", dir(private_attribute))
        self.assertIn("per-instance", private_attribute.PrivateObjSuper.__doc__)


class TestInteraction(unittest.TestCase):
    def test_class_attribute_is_invisible_here(self):
        # Same name: PrivateObjSuper misses the class value; PrivateSuper sees it.
        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_c"]
            _c = "classlevel"

        class C(P, use_parents=True):
            __private_attrs__ = []

            def read_inst(self):
                return private_attribute.PrivateObjSuper(C, P, self)._c

            def read_cls(self):
                return private_attribute.PrivateSuper(C, P, P, self)._c

        obj = C()
        with self.assertRaises(AttributeError) as cm:
            obj.read_inst()
        self.assertIn("'PrivateObjSuper' object has no attribute '_c'", str(cm.exception))
        self.assertEqual(obj.read_cls(), "classlevel")

    def test_intermediate_class_does_not_forward(self):
        class Grand(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_g"]

            def __init__(self):
                self._g = "G"

        class Mid(Grand):
            __private_attrs__ = []

        class Leaf(Mid):
            __private_attrs__ = []

            def read_grand(self):
                return private_attribute.PrivateObjSuper(Leaf, Grand, self)._g

            def read_mid(self):
                return private_attribute.PrivateObjSuper(Leaf, Mid, self)._g

        leaf = Leaf()
        self.assertEqual(leaf.read_grand(), "G")
        with self.assertRaises(AttributeError) as cm:
            leaf.read_mid()
        # The name is private on Grand, so Mid passes the privacy check, but
        # Mid never stored a value of its own.
        self.assertIn("'PrivateObjSuper' object has no attribute '_g'", str(cm.exception))

    def test_each_class_keeps_its_own_value(self):
        class G(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_v"]

            def __init__(self):
                self._v = "G"

            def gv(self):
                return self._v

        class M(G):
            __private_attrs__ = ["_v"]

            def __init__(self):
                super().__init__()
                self._v = "M"

            def mv(self):
                return self._v

        class L(M):
            __private_attrs__ = []

            def rg(self):
                return private_attribute.PrivateObjSuper(L, G, self)._v

            def rm(self):
                return private_attribute.PrivateObjSuper(L, M, self)._v

        obj = L()
        self.assertEqual((obj.gv(), obj.mv(), obj.rg(), obj.rm()), ("G", "M", "G", "M"))

    def test_multiple_inheritance_reads_each_parent(self):
        class L(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_v"]

            def __init__(self):
                self._v = "L"

        class R(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_v"]

            def __init__(self):
                self._v = "R"

        class D(L, R):
            __private_attrs__ = []

            def __init__(self):
                L.__init__(self)
                R.__init__(self)

            def read_l(self):
                return private_attribute.PrivateObjSuper(D, L, self)._v

            def read_r(self):
                return private_attribute.PrivateObjSuper(D, R, self)._v

        obj = D()
        self.assertEqual(obj.read_l(), "L")
        self.assertEqual(obj.read_r(), "R")

    def test_unrelated_base_is_rejected_and_real_parents_work(self):
        class Root(private_attribute.PrivateAttrBase):
            __private_attrs__ = []

        class Sibling(Root):
            __private_attrs__ = ["_g"]

            def __init__(self):
                self._g = "sib"

        class B(Root):
            __private_attrs__ = ["_g"]

            def __init__(self):
                self._g = "b"

        class A(Sibling, B):
            __private_attrs__ = []

            def __init__(self):
                Sibling.__init__(self)
                B.__init__(self)

            def read_b(self):
                return private_attribute.PrivateObjSuper(A, B, self)._g

            def read_sib(self):
                return private_attribute.PrivateObjSuper(A, Sibling, self)._g

            def read_root(self):
                return private_attribute.PrivateObjSuper(A, Root, self)._g

        obj = A()
        self.assertEqual(obj.read_b(), "b")
        self.assertEqual(obj.read_sib(), "sib")
        with self.assertRaises(AttributeError) as cm:
            obj.read_root()
        self.assertIn("has no private attribute '_g'", str(cm.exception))

    def test_instance_descriptor_is_not_bound(self):
        class Descr:
            def __get__(self, inst, owner):
                return "bound"

        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_d"]

            def __init__(self):
                self._d = Descr()

            def getd(self):
                return self._d

        class C(P):
            __private_attrs__ = []

            def read(self):
                return private_attribute.PrivateObjSuper(C, P, self)._d

        obj = C()
        self.assertIsInstance(obj.getd(), Descr)
        self.assertIs(obj.read(), obj.getd())

    def test_class_data_descriptor_does_not_hide_instance_value(self):
        class Data:
            def __get__(self, inst, owner):
                return "DATA"

            def __set__(self, inst, value):
                raise AssertionError("descriptor __set__ should not run")

        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_d"]

            def __init__(self):
                self._d = "instance-value"

            def getd(self):
                return self._d

            @classmethod
            def install(cls, value):
                cls._d = value

        class C(P):
            __private_attrs__ = []

            def read(self):
                return private_attribute.PrivateObjSuper(C, P, self)._d

        obj = C()
        P.install(Data())
        # Ordinary lookup honors the data descriptor; PrivateObjSuper does not.
        self.assertEqual(obj.getd(), "DATA")
        self.assertEqual(obj.read(), "instance-value")

    def test_parent_private_func_round_trip(self):
        def gen(obj_id, name):
            return f"k_{obj_id}_{name}"

        class P(private_attribute.PrivateAttrBase, private_func=gen):
            __private_attrs__ = ["_p"]

            def __init__(self, value):
                self._p = value

        class C(P):
            __private_attrs__ = []

            def read(self):
                return private_attribute.PrivateObjSuper(C, P, self)._p

        self.assertEqual(C(123).read(), 123)

    def test_private_func_errors_propagate(self):
        def bad(obj_id, name):
            return 5

        def boom(obj_id, name):
            raise RuntimeError("boom")

        class Bad(private_attribute.PrivateAttrBase, private_func=bad):
            __private_attrs__ = ["_p"]

        class BadChild(Bad):
            __private_attrs__ = []

            def read(self):
                return private_attribute.PrivateObjSuper(BadChild, Bad, self)._p

        with self.assertRaises(TypeError) as cm:
            BadChild().read()
        self.assertIn("must return a string", str(cm.exception))

        class Boom(private_attribute.PrivateAttrBase, private_func=boom):
            __private_attrs__ = ["_p"]

        class BoomChild(Boom):
            __private_attrs__ = []

            def read(self):
                return private_attribute.PrivateObjSuper(BoomChild, Boom, self)._p

        with self.assertRaises(RuntimeError):
            BoomChild().read()


class TestClassObjectBinding(unittest.TestCase):
    def test_descriptor_on_a_class_object_and_reentry(self):
        class Tag:
            def __get__(self, inst, owner):
                return ("tag", inst is None, owner.__name__)

        class Reenter:
            def __get__(self, inst, owner):
                # Re-enter the parent metaclass's private storage for this class.
                return owner.touch()

        class PrivateMeta(type):
            __new__ = private_attribute.create_new()

        class MetaParent(type, metaclass=PrivateMeta):
            __private_attrs__ = ["_slot", "_fn", "_other"]

            def __init__(cls, name, bases, ns, **kw):
                super().__init__(name, bases, ns)
                cls._other = "other"
                cls._slot = Tag()

                def fn(arg):
                    return "plain-" + str(arg)

                cls._fn = fn

            def touch(cls):
                return cls._other

        class MetaChild(MetaParent):
            __private_attrs__ = []

            def grab_slot(cls, target):
                proxy = private_attribute.PrivateObjSuper(MetaChild, MetaParent, target)
                return proxy._slot

            def grab_fn(cls, target):
                proxy = private_attribute.PrivateObjSuper(MetaChild, MetaParent, target)
                return proxy._fn

        class Subject(metaclass=MetaChild):
            pass

        self.assertEqual(Subject.grab_slot(Subject), ("tag", True, "Subject"))
        self.assertEqual(Subject.grab_fn(Subject)(1), "plain-1")

        class MetaParent2(type, metaclass=PrivateMeta):
            __private_attrs__ = ["_slot", "_other"]

            def __init__(cls, name, bases, ns, **kw):
                super().__init__(name, bases, ns)
                cls._other = "other"
                cls._slot = Reenter()

            def touch(cls):
                return cls._other

        class MetaChild2(MetaParent2):
            __private_attrs__ = []

            def grab(cls, target):
                proxy = private_attribute.PrivateObjSuper(MetaChild2, MetaParent2, target)
                return proxy._slot

        class Subject2(metaclass=MetaChild2):
            pass

        self.assertEqual(Subject2.grab(Subject2), "other")


class TestValidation(unittest.TestCase):
    def test_argument_count_and_keywords(self):
        obj = Child(1, 2)
        with self.assertRaises(TypeError) as cm:
            private_attribute.PrivateObjSuper(Child, Parent)
        self.assertIn("PrivateObjSuper() takes exactly 3 arguments (2 given)", str(cm.exception))
        with self.assertRaises(TypeError) as cm:
            private_attribute.PrivateObjSuper(Child, Parent, obj, 1)
        self.assertIn("takes exactly 3 arguments (4 given)", str(cm.exception))
        with self.assertRaises(TypeError) as cm:
            private_attribute.PrivateObjSuper(Child, Parent, obj, extra=1)
        self.assertIn("takes no keyword arguments", str(cm.exception))
        with self.assertRaises(TypeError):
            private_attribute.PrivateObjSuper(now_class=Child, parent_class=Parent, obj=obj)

    def test_types_must_be_private_classes(self):
        obj = Child(1, 2)
        with self.assertRaises(TypeError) as cm:
            private_attribute.PrivateObjSuper(int, Parent, obj)
        self.assertIn("now_class must be private class", str(cm.exception))
        with self.assertRaises(TypeError) as cm:
            private_attribute.PrivateObjSuper(Child, int, obj)
        self.assertIn("parent_class must be private class", str(cm.exception))

    def test_non_types(self):
        obj = Child(1, 2)
        with self.assertRaises(TypeError) as cm:
            private_attribute.PrivateObjSuper(obj, Parent, obj)
        self.assertIn("now_class must be a type", str(cm.exception))
        with self.assertRaises(TypeError) as cm:
            private_attribute.PrivateObjSuper(Child, obj, obj)
        self.assertIn("parent_class must be a type", str(cm.exception))

    def test_parent_must_actually_be_a_parent(self):
        obj = Child(1, 2)
        with self.assertRaises(TypeError) as cm:
            private_attribute.PrivateObjSuper(Child, Child, obj)
        self.assertIn("parent_class must be parent of now_class", str(cm.exception))

        class Other(private_attribute.PrivateAttrBase):
            __private_attrs__ = []

        with self.assertRaises(TypeError):
            private_attribute.PrivateObjSuper(Child, Other, obj)

    def test_obj_must_be_an_instance_of_now_class(self):
        class Other(Parent):
            __private_attrs__ = []

        obj = Child(1, 2)
        for bad in (object(), Parent(1), Other(1), None):
            with self.assertRaises(TypeError):
                obj.read_cm(bad)

class TestOwnershipAndEdges(unittest.TestCase):
    def test_outside_code_and_parent_code_are_denied(self):
        obj = Child(1, 2)
        proxy = obj.make()
        with self.assertRaises(AttributeError) as cm:
            proxy._x
        self.assertIn("outside the class", str(cm.exception))
        self.assertFalse(hasattr(proxy, "_x"))

        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_x"]

            def __init__(self):
                self._x = 7

            def sneak(self):
                return private_attribute.PrivateObjSuper(C, P, self)._x

        class C(P):
            __private_attrs__ = []

            def ok(self):
                return private_attribute.PrivateObjSuper(C, P, self)._x

        made = C()
        self.assertEqual(made.ok(), 7)
        with self.assertRaises(AttributeError) as cm:
            made.sneak()
        self.assertIn("outside the class", str(cm.exception))

    def test_external_helper_and_late_method_are_denied(self):
        def helper(obj):
            return private_attribute.PrivateObjSuper(C, P, obj)._x

        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_x"]

            def __init__(self):
                self._x = 3

        class C(P):
            __private_attrs__ = []

            def read(self):
                return helper(self)

        with self.assertRaises(AttributeError):
            C().read()

        def late(self):
            return private_attribute.PrivateObjSuper(C, P, self)._x

        C.late = late
        with self.assertRaises(AttributeError):
            C().late()

    def test_missing_public_hasattr_and_dir(self):
        obj = Child(1, 2)
        with self.assertRaises(AttributeError) as cm:
            obj.read_missing()
        self.assertEqual(str(cm.exception), "'PrivateObjSuper' object has no attribute '_y'")
        with self.assertRaises(AttributeError) as cm:
            obj.read_public()
        self.assertIn("'Parent' has no private attribute 'public'", str(cm.exception))
        present, missing, public, listed = obj.flags()
        self.assertEqual((present, missing, public, listed), (True, False, False, False))

    def test_non_string_attribute_name(self):
        class C(Parent):
            __private_attrs__ = []

            def go(self):
                proxy = private_attribute.PrivateObjSuper(C, Parent, self)
                return getattr(proxy, 1)

        with self.assertRaises(TypeError) as cm:
            C(1).go()
        self.assertIn("attribute name must be string", str(cm.exception))

    def test_assignment_and_deletion_do_not_change_the_parent_value(self):
        obj = Child(1, 2)
        with self.assertRaises(AttributeError) as cm:
            obj.try_set()
        self.assertIn("no attribute '_x'", str(cm.exception))
        self.assertEqual(obj.get_x(), 1)
        with self.assertRaises(AttributeError):
            obj.try_del()
        self.assertEqual(obj.get_x(), 1)

    def test_cannot_subclass_weakref_or_replace_the_export(self):
        with self.assertRaises(TypeError):
            type("Sub", (private_attribute.PrivateObjSuper,), {})
        with self.assertRaises(TypeError):
            weakref.ref(Child(1, 2).make())
        with self.assertRaises(AttributeError):
            private_attribute.PrivateObjSuper = None

    def test_private_attr_base_declares_no_names(self):
        class C(private_attribute.PrivateAttrBase):
            __private_attrs__ = []

            def read(self):
                proxy = private_attribute.PrivateObjSuper(C, private_attribute.PrivateAttrBase, self)
                return proxy.nope

        with self.assertRaises(AttributeError) as cm:
            C().read()
        self.assertIn("'PrivateAttrBase' has no private attribute 'nope'", str(cm.exception))


class TestPrivateClassCache(unittest.TestCase):
    def test_new_metaclass_after_collected_classes(self):
        # Regression: the private-class cache used to keep a freed type's
        # answer, so a metaclass allocated at that address was rejected as
        # "already private" and PrivateObjSuper could not be built on it.
        for i in range(40):
            class P(private_attribute.PrivateAttrBase):
                __private_attrs__ = ["_x"]

                def __init__(self, value):
                    self._x = value

            class C(P):
                __private_attrs__ = []

                def read(self):
                    return private_attribute.PrivateObjSuper(C, P, self)._x

            self.assertEqual(C(i).read(), i)
        gc.collect()

        class PrivateMeta(type):
            __new__ = private_attribute.create_new()

        class MetaParent(type, metaclass=PrivateMeta):
            __private_attrs__ = ["_v"]

            def __init__(cls, name, bases, ns, **kw):
                super().__init__(name, bases, ns)
                cls._v = "from-parent"

        class MetaChild(MetaParent):
            __private_attrs__ = []

            def read(cls, target):
                proxy = private_attribute.PrivateObjSuper(MetaChild, MetaParent, target)
                return proxy._v

        class Subject(metaclass=MetaChild):
            pass

        self.assertEqual(Subject.read(Subject), "from-parent")


class TestGC(unittest.TestCase):
    def test_cycle_through_instance_dict_is_collected(self):
        class Payload:
            pass

        class P(private_attribute.PrivateAttrBase):
            __private_attrs__ = ["_x"]

            def __init__(self):
                self._x = 1

        class C(P):
            __private_attrs__ = []

            def make(self):
                return private_attribute.PrivateObjSuper(C, P, self)

        payload = Payload()
        obj = C()
        proxy = obj.make()
        obj.loop = proxy
        obj.payload = payload
        pref = weakref.ref(payload)
        oref = weakref.ref(obj)
        del payload, proxy, obj
        gc.collect()
        self.assertIsNone(pref())
        self.assertIsNone(oref())


if __name__ == "__main__":
    unittest.main()
