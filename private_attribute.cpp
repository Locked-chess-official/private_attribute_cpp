#define PY_SSIZE_T_CLEAN
#ifdef Py_LIMITED_API
#undef Py_LIMITED_API
#endif
#include <Python.h>
#include <frameobject.h>
#include <structmember.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <random>
#include <mutex>
#include <shared_mutex>
#include "picosha2.h"
#include <functional>
#include <memory>
#include <new>

// python >= 3.15 has PyAnyDict_Check. Under 3.15 defined it as PyDict_Check
#if PY_VERSION_HEX < 0x030F0000
#define PyAnyDict_Check(op) PyDict_Check(op)
#endif

static const auto module_running_time = std::chrono::system_clock::now();

static std::string
time_to_string() noexcept
{
    std::time_t original_time = std::chrono::system_clock::to_time_t(module_running_time);
    std::tm original_tm = *std::localtime(&original_time);
    std::stringstream ss;
    ss << std::put_time(&original_tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static const auto module_running_time_string = time_to_string();

static const std::string compile_time = __DATE__ " " __TIME__;

class AllPyobjectAttrCacheKey
{
private:
    uintptr_t obj_id;
    std::string attr_onehash;
    std::string another_string_hash;
public:
    AllPyobjectAttrCacheKey(uintptr_t obj_id, const std::string& attr_name) noexcept : obj_id(obj_id) {
        std::string one_name = "_" + std::to_string(obj_id) + "_" + compile_time + attr_name;
        std::string another_name = "_" + module_running_time_string + attr_name;
        picosha2::hash256_hex_string(one_name, attr_onehash);
        picosha2::hash256_hex_string(another_name, another_string_hash);
    }

    std::size_t gethash() const noexcept {
        std::size_t h1 = std::hash<uintptr_t>{}(obj_id);
        std::size_t h2 = std::hash<std::string>{}(attr_onehash);
        std::size_t h3 = std::hash<std::string>{}(another_string_hash);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }

    bool operator==(const AllPyobjectAttrCacheKey& other) const noexcept {
        return this->obj_id == other.obj_id && this->attr_onehash == other.attr_onehash && this->another_string_hash == other.another_string_hash;
    }
};

class TwoStringTuple
{
private:
    std::string first;
    std::string second;
public:
    TwoStringTuple(const std::string& first, const std::string& second) noexcept : first(first), second(second) {}
    bool operator==(const TwoStringTuple& other) const noexcept {
        return this->first == other.first && this->second == other.second;
    }

    std::size_t gethash() const noexcept {
        std::size_t h1 = std::hash<std::string>{}(first);
        std::size_t h2 = std::hash<std::string>{}(second);
        return h1 ^ (h2 << 1);
    }
};

namespace std {
    template<>
    struct hash<AllPyobjectAttrCacheKey> {
        std::size_t operator()(const AllPyobjectAttrCacheKey& key) const noexcept {
            return key.gethash();
        }
    };

    template<>
    struct hash<TwoStringTuple> {
        std::size_t operator()(const TwoStringTuple& key) const noexcept {
            return key.gethash();
        }
    };
}

struct PyObjectStorage
{
private:
    PyObject* obj = NULL;

public:
    PyObjectStorage() noexcept = default;
    PyObjectStorage(PyObject* obj) noexcept : obj(obj) {
        Py_XINCREF(obj);
    }
    // Construct a PyObjectStorage that takes ownership of a strong reference
    // already held by the caller (a "new reference"). The reference count is
    // NOT incremented; the stored object's ownership is simply transferred
    // into the storage, and the destructor will release it exactly once.
    static PyObjectStorage from_new_ref(PyObject* obj) noexcept {
        PyObjectStorage storage;
        storage.obj = obj;
        return storage;
    }
    static PyObjectStorage from_new_ref(PyCodeObject* obj) noexcept {
        return from_new_ref(reinterpret_cast<PyObject*>(obj));
    }
    // support for PyCodeObject*
    PyObjectStorage(PyCodeObject* obj) noexcept : obj(reinterpret_cast<PyObject*>(obj)) {
        Py_XINCREF(obj);
    }
    PyObjectStorage(const PyObjectStorage& other) noexcept : obj(other.obj) {
        Py_XINCREF(obj);
    }
    PyObjectStorage(PyObjectStorage&& other) noexcept : obj(other.obj) {
        other.obj = NULL;
    }
    PyObjectStorage& operator=(const PyObjectStorage& other) noexcept {
        if (this == &other) {
            return *this;
        }
        PyObject* new_obj = other.obj;
        Py_XINCREF(new_obj);
        Py_XDECREF(obj);
        obj = new_obj;
        return *this;
    }
    PyObjectStorage& operator=(PyObjectStorage&& other) noexcept {
        if (this != &other) {
            Py_XDECREF(obj);
            obj = other.obj;
            other.obj = NULL;
        }
        return *this;
    }
    PyObjectStorage& operator=(PyObject* new_obj) noexcept {
        Py_XINCREF(new_obj);
        Py_XDECREF(obj);
        obj = new_obj;
        return *this;
    }
    PyObjectStorage& operator=(PyCodeObject* new_obj) noexcept {
        Py_XINCREF(reinterpret_cast<PyObject*>(new_obj));
        Py_XDECREF(obj);
        obj = reinterpret_cast<PyObject*>(new_obj);
        return *this;
    }
    ~PyObjectStorage() noexcept {
        // if python has exit we don't do anything
        if (Py_IsInitialized()) {
            Py_XDECREF(obj);
        }
    }

    operator PyObject*() const noexcept {
        return obj;
    }

    explicit operator bool() const noexcept {
        return obj != NULL;
    }

    PyObject* get() const noexcept {
        return obj;
    }

    PyObject** getpointer() noexcept {
        return &obj;
    }
};

// python under 3.13 doesn't have PyDict_ContainsString, so we implement it ourselves
#if PY_VERSION_HEX < 0x030D0000
static int
PyDict_ContainsString(PyObject *op, const char *key) noexcept
{
    PyObjectStorage key_obj = PyObjectStorage::from_new_ref(PyUnicode_FromString(key));
    if (key_obj.get() == NULL) {
        return -1;
    }
    int res = PyDict_Contains(op, key_obj);
    return res;
}
#endif

typedef struct PrivateAttrCreationData PrivateAttrCreationData;
static std::string default_random_string(uintptr_t obj_id, const std::string& attr_name) noexcept;

struct RestorePythonExceptionResult {
    std::string value;
    bool ok = true;
};
static RestorePythonExceptionResult custom_random_string(uintptr_t obj_id, const std::string& attr_name, PyObject* func) noexcept;

// Result of need_analyse_type(). A Python exception may be pending when the
// result is Error. Every caller MUST propagate an Error result up the call
// chain (do not silently swallow the pending exception with PyErr_Clear).
enum class AnalyseStatus {
    No = 0,     // the type does not need private-attribute analysis
    Yes = 1,    // the type needs private-attribute analysis
    Error = -1  // a Python exception is pending
};
static AnalyseStatus need_analyse_type(PyObject* type) noexcept;
static void ensure_tp(PyTypeObject* type_instance) noexcept;

// v3: special-method slot structs (descriptors installed as __getattribute__ /
// __getattr__ / __setattr__ / __delattr__ / __del__ and, for metaclasses,
// __call__). Installed into heap types so CPython's update_one_slot rewires the
// tp_getattro / tp_setattro / tp_call slots to the standard slot dispatchers.
// The full struct definition and type objects live in the "v3 slot structs"
// section below; these are only forward declarations used by earlier helpers.
static setattrofunc original_type_tp_setattro = 0;
static PyObject* make_type_slot(const char* name, PyObject* callback, PyObject* owner=NULL) noexcept;
static PyObject* make_metaclass_slot(const char* name, PyObject* owner, PyObject* callback) noexcept;
// inject the five special-method structs into a class namespace (attrs dict)
static bool inject_type_slots(PyObject* attrs) noexcept;
// install the six special-method structs into a registered (heap) metaclass dict
static bool install_metaclass_slots(PyTypeObject* metaclass) noexcept;
static PyObject* register_metaclass_head(PyObject* metaclass) noexcept;
// write a new user callback into an installed slot struct's callback field
static bool update_slot_callback(PyTypeObject* owner, const char* name, PyObject* callback, int is_metaclass) noexcept;
// public non-private-name cache helpers
static bool is_confirmed_public(uintptr_t obj_id, const std::string& name) noexcept;
static void mark_confirmed_public(uintptr_t obj_id, const std::string& name) noexcept;
static void clear_public_obj(uintptr_t obj_id) noexcept;
static bool is_registered_type(PyObject* type) noexcept;
static bool is_private_meta_type(PyObject* typ) noexcept;

struct is_private_class_control {
    PyObjectStorage weakref; // weakref to the type object
    bool result; // cached need_analyse_type() answer (true == needs analysis)
};

namespace {
    namespace AllData {
        static std::unordered_map<AllPyobjectAttrCacheKey, std::string> cache;
        static std::unordered_set<std::string> all_exist_name;
        static std::unordered_map<uintptr_t, std::vector<AllPyobjectAttrCacheKey>> obj_attr_keys;
        static std::shared_mutex cache_mutex;
        namespace {
            static std::unordered_map<uintptr_t, std::unordered_map<std::string, PyObjectStorage>> type_attr_dict;
        };
        static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t, PyObjectStorage>> type_allowed_code_map;
        // 2.2.1: while a type is still being created (its id is not yet written
        // into all_type_attr_set), tmp_creation_data points at the in-flight
        // PrivateAttrCreationData. Accesses that happen inside type.tp_new (e.g.
        // a parent's __init_subclass__) read the private-attribute set and the
        // code-object map directly from tmp_creation_data until postprocess
        // promotes the data into the real AllData tables.
        static std::mutex type_creation_lock_mutex;
        static bool type_creation_locked = false;
        static PrivateAttrCreationData *tmp_creation_data = nullptr;

        static std::unordered_map<uintptr_t, std::unique_ptr<std::shared_mutex>> all_type_mutex;
        static std::unordered_map<uintptr_t, PyObjectStorage> type_need_call;
        static std::unordered_map<uintptr_t, std::unordered_set<TwoStringTuple>> all_type_attr_set;
        namespace {
            static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t,
                std::unordered_map<std::string, PyObjectStorage>>> all_object_attr, all_type_subclass_attr;
        };
        static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t, std::unique_ptr<std::shared_mutex>>>
            all_object_mutex, all_type_subclass_mutex;
        static std::mutex object_creatmutex_mutex, type_createmutex_mutex, type_parenttype_createmutex_mutex;
        static std::unordered_map<uintptr_t, std::vector<uintptr_t>> all_type_parent_id;
        // all type tp_traverse map
        static std::unordered_map<uintptr_t, traverseproc> all_type_traverse;
        // all type tp_clear map
        static std::unordered_map<uintptr_t, inquiry> all_type_clear;
        // CPython's default slot functions for heap types (subtype_traverse /
        // subtype_clear). type_new assigns them to every heap type, so we capture
        // the exact pointers from the first heap type created through tp_new
        // (PrivateAttrBase). The get_*_need_* walkers must treat these delegating
        // defaults as "not a real original" and keep walking upward.
        static traverseproc captured_subtype_traverse = NULL;
        static inquiry captured_subtype_clear = NULL;

        static std::shared_mutex all_register_new_metaclass_mutex;
        static std::unordered_map<uintptr_t, PyObjectStorage> all_register_type_weak_ref;
        static std::unordered_map<uintptr_t, uintptr_t> weakref_type;
        // v3: public non-private-name cache. Keyed by object id, value is the set
        // of attribute names already confirmed non-private for that object.
        // Once a name is present, attribute access goes straight to the callback
        // without re-classifying. Guarded by public_cache_mutex.
        static std::unordered_map<uintptr_t, std::unordered_set<std::string>> public_attr_cache;
        static std::shared_mutex public_cache_mutex;
        // Single dict-backed strong-ref cache shared by the module itself (keyed by
        // kModuleSelfKey) and by PrivateAttrBase (keyed by kPrivateAttrBaseKey),
        // so clean_all_storages can release everything at interpreter shutdown.
        static std::unordered_map<uintptr_t, PyObjectStorage> store_module_self;
        static constexpr uintptr_t kModuleSelfKey = 0;
        static constexpr uintptr_t kPrivateAttrBaseKey = 1;
        static std::shared_mutex is_private_mutex;
        static std::unordered_map<uintptr_t, is_private_class_control> is_private_cache;
        static std::unordered_map<uintptr_t, uintptr_t> is_private_cache_weakref_dict;
    };
};

static PyObject*
is_private_class_weakref_callback(PyObject* /*self*/, PyObject* weakref)
{
    std::unique_lock<std::shared_mutex> lock(AllData::is_private_mutex);
    uintptr_t weakref_id = reinterpret_cast<uintptr_t>(weakref);
    auto it = AllData::is_private_cache_weakref_dict.find(weakref_id);
    if (it == AllData::is_private_cache_weakref_dict.end()) {
        // Already evicted (e.g. by clean_all_storages during interpreter
        // shutdown). Treat as a no-op rather than raising a spurious error.
        Py_RETURN_NONE;
    }
    uintptr_t type_id = AllData::is_private_cache_weakref_dict.erase(weakref_id);
    AllData::is_private_cache.erase(type_id);
    Py_RETURN_NONE;
}

static PyMethodDef is_private_class_methods = {
    "is_private_class_weakref_callback",
    (PyCFunction)is_private_class_weakref_callback,
    METH_O,
    NULL
};

static int
store_cache_for_is_private_class(PyObject* type, bool result)
{
    if (!PyType_Check(type)) {
        PyErr_SetString(PyExc_TypeError, "type must be a type object");
        return -1;
    }
    PyObjectStorage func = PyObjectStorage::from_new_ref(PyCFunction_New(&is_private_class_methods, NULL));
    if (!func) {
        return -1;
    }
    PyObjectStorage weakref = PyObjectStorage::from_new_ref(PyWeakref_NewRef((PyObject*)type, (PyObject*)func));
    if (!weakref) {
        return -1;
    }
    std::unique_lock<std::shared_mutex> lock(AllData::is_private_mutex);
    AllData::is_private_cache[reinterpret_cast<uintptr_t>(type)] = {weakref, result};
    AllData::is_private_cache_weakref_dict[reinterpret_cast<uintptr_t>(weakref.get())] = reinterpret_cast<uintptr_t>(type);
    return 0;
}

struct analyse_cache_lookup_result {
    bool found;
    bool result;
};

static analyse_cache_lookup_result
find_is_private_class(PyObject* type)
{
    std::shared_lock<std::shared_mutex> lock(AllData::is_private_mutex);
    auto it = AllData::is_private_cache.find(reinterpret_cast<uintptr_t>(type));
    if (it == AllData::is_private_cache.end()) {
        return {false, false};
    }
    return {true, it->second.result};
}

struct PrivateAttrCreationData
{
    std::string class_name;
    PyObjectStorage attrs_copy = (PyObject*)nullptr;
    std::unordered_set<TwoStringTuple> private_attrs_set;
    std::unordered_map<uintptr_t, PyObjectStorage> code_map;
    std::unordered_set<std::string> private_attrs_vector_string;
    std::vector<uintptr_t> all_need_analyse_base;
    std::unordered_map<std::string, PyObjectStorage> need_remove_itself;
    PyObjectStorage private_func = (PyObject*)nullptr;
    PyObjectStorage base_kwds = (PyObject*)nullptr;
    PyObjectStorage name = (PyObject*)nullptr;
    PyObjectStorage bases = (PyObject*)nullptr;
    bool use_parents = false;
    bool cleared = false;
    std::unique_lock<std::mutex> lock_;
    bool locked = false;
    bool installed = false;
    PrivateAttrCreationData() noexcept : lock_(::AllData::type_creation_lock_mutex, std::defer_lock) {}

    void lock() noexcept {
        if (!locked) {
            lock_.lock();
            AllData::type_creation_locked = true;
            locked = true;
            AllData::tmp_creation_data = this;
        }
    }

    void unlock() noexcept {
        if (locked) {
            AllData::tmp_creation_data = nullptr;
            AllData::type_creation_locked = false;
            locked = false;
            lock_.unlock();
        }
    }

    void clear() noexcept {
        if (cleared) {
            return;
        }
        name = (PyObject*)nullptr;
        bases = (PyObject*)nullptr;
        attrs_copy = (PyObject*)nullptr;
        private_func = (PyObject*)nullptr;
        base_kwds = (PyObject*)nullptr;

        need_remove_itself.clear();
        code_map.clear();
        if (locked) {
            unlock();
        }
        cleared = true;
    }

    bool install(PyObject* new_type) noexcept {
        if (installed) return true;
        if (!new_type) {
            return false;
        }
        if (!PyType_Check(new_type)) {
            PyErr_SetString(PyExc_TypeError, "created object is not a type");
            return false;
        }

        PyTypeObject* type_instance = (PyTypeObject*)new_type;
        uintptr_t type_id = (uintptr_t)(type_instance);
        int tp_dict_has_static_attr = PyDict_ContainsString(type_instance->tp_dict, "__static_attributes__");
        if (tp_dict_has_static_attr < 0) {
            return false;
        }
        if (tp_dict_has_static_attr) {
            PyErr_Format(PyExc_SystemError,
                "%s (%d): '__static_attributes__' will expose private attribute names and cannot be used in private attribute types", __FILE__, __LINE__);
            return false;
        }
        ::AllData::all_type_attr_set[type_id] = private_attrs_set;
        ensure_tp(type_instance);
        if (::AllData::type_attr_dict.find(type_id) == ::AllData::type_attr_dict.end()) {
            ::AllData::type_attr_dict[type_id] = {};
        }
        // iter mro and put in all_type_parent_id
        PyObject* mro = type_instance->tp_mro;
        Py_ssize_t mro_size = PyTuple_GET_SIZE(mro);
        std::vector<uintptr_t> mro_vector;
        for (Py_ssize_t i = 1; i < mro_size; i++) {
            PyObject* item = PyTuple_GET_ITEM(mro, i);
            if (!item || !PyType_Check(item)) {
                continue;
            }
            AnalyseStatus analyse_status = need_analyse_type(item);
            if (analyse_status == AnalyseStatus::Error) {
                return false;
            }
            if (analyse_status == AnalyseStatus::No) {
                continue;
            }
            mro_vector.push_back((uintptr_t)item);
        }
        ::AllData::all_type_parent_id[type_id] = mro_vector;
        ::AllData::all_object_mutex[type_id].clear();
        ::AllData::all_type_mutex[type_id] = std::make_unique<std::shared_mutex>();
        ::AllData::all_object_attr[type_id] = {};
        ::AllData::all_type_subclass_attr[type_id] = {};
        ::AllData::all_type_subclass_mutex[type_id].clear();
        ::AllData::type_allowed_code_map[type_id] = code_map;

        for (uintptr_t i: mro_vector) {
            if (::AllData::all_type_subclass_attr.find(i) == ::AllData::all_type_subclass_attr.end()) {
                ::AllData::all_type_subclass_attr[i] = {};
            }
            if (::AllData::all_type_subclass_mutex.find(i) == ::AllData::all_type_subclass_mutex.end()) {
                ::AllData::all_type_subclass_mutex[i].clear();
            }
            // Do not wipe a parent's shadow already written for this class during
            // __init_subclass__ (e.g. `cls._value = ...` inside the parent's
            // __init_subclass__ runs inside tp_new, before postprocess).
            if (::AllData::all_type_subclass_attr[i].find(type_id) == ::AllData::all_type_subclass_attr[i].end()) {
                ::AllData::all_type_subclass_attr[i][type_id] = {};
            }
            if (::AllData::all_type_subclass_mutex[i].find(type_id) == ::AllData::all_type_subclass_mutex[i].end()) {
                ::AllData::all_type_subclass_mutex[i][type_id] = std::make_unique<std::shared_mutex>();
            }
        }

        for (auto& [key, value]: need_remove_itself) {
            std::string final_key;
            if (private_func) {
                auto private_name_result = custom_random_string(type_id, key, private_func);
                if (!private_name_result.ok) {
                    return false;
                }
                final_key = private_name_result.value;
            } else {
                final_key = default_random_string(type_id, key);
            }
            ::AllData::type_attr_dict[type_id][final_key] = value;
        }
        if (private_func) {
            ::AllData::type_need_call[type_id] = private_func;
        }
        unlock();
        installed = true;
        return true;
    }

    ~PrivateAttrCreationData() noexcept {
        clear();
    }
};

static bool PrivateAttrType_preprocess(PyObject* args, PyObject* kwds, PrivateAttrCreationData& data, const std::string& func_name) noexcept;
static bool
try_install_pending_creation(PyObject* new_type) noexcept
{
    // Finish a pending private-attribute creation if one is still
    // outstanding. prepare()/PrivateAttrType_new leave tmp_creation_data
    // locked (type_creation_locked == true) until install() runs; if
    // creation is interrupted (e.g. an exception in __init_subclass__) the
    // type exists but is not yet registered in all_type_attr_set. The
    // metaclass getattr/setattr/call slots check this first and promote the
    // half-created type before proceeding.
    if (!new_type || !::AllData::tmp_creation_data) {
        return true;;
    }
    if (::AllData::type_creation_locked &&
        ::AllData::all_type_attr_set.find((uintptr_t)new_type) == ::AllData::all_type_attr_set.end()) {
        return ::AllData::tmp_creation_data->install(new_type);
    }
    return true;
}

static inline void
object_create_mutex(uintptr_t final_id, uintptr_t obj_id) noexcept
{
    // get object_creatmutex_mutex
    std::lock_guard<std::mutex> lock(AllData::object_creatmutex_mutex);
    if (::AllData::all_object_attr[final_id].find(obj_id) == ::AllData::all_object_attr[final_id].end()) {
        ::AllData::all_object_attr[final_id][obj_id] = {};
    }
    if (::AllData::all_object_mutex.find(final_id) == ::AllData::all_object_mutex.end()) {
        ::AllData::all_object_mutex[final_id].clear();
    }
    if (::AllData::all_object_mutex[final_id].find(obj_id) == ::AllData::all_object_mutex[final_id].end()) {
        ::AllData::all_object_mutex[final_id][obj_id] = std::make_unique<std::shared_mutex>();
    }
}

static inline void
type_create_mutex(uintptr_t typ_id) noexcept {
    std::lock_guard<std::mutex> lock(AllData::type_createmutex_mutex);
    if (::AllData::type_attr_dict.find(typ_id) == ::AllData::type_attr_dict.end()) {
        ::AllData::type_attr_dict[typ_id] = {};
    }
    if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
        ::AllData::all_type_mutex[typ_id] = std::make_unique<std::shared_mutex>();
    }
}

static inline void
type_parenttype_create_mutex(uintptr_t final_id, uintptr_t typ_id) noexcept
{
    std::lock_guard<std::mutex> lock(AllData::type_parenttype_createmutex_mutex);
    if (::AllData::all_type_subclass_attr.find(final_id) == ::AllData::all_type_subclass_attr.end()) {
        ::AllData::all_type_subclass_attr[final_id] = {};
    }
    if (::AllData::all_type_subclass_attr[final_id].find(typ_id) == ::AllData::all_type_subclass_attr[final_id].end()) {
        ::AllData::all_type_subclass_attr[final_id][typ_id] = {};
    }
    if (::AllData::all_type_subclass_mutex.find(final_id) == ::AllData::all_type_subclass_mutex.end()) {
        ::AllData::all_type_subclass_mutex[final_id].clear();
    }
    if (::AllData::all_type_subclass_mutex[final_id].find(typ_id) == ::AllData::all_type_subclass_mutex[final_id].end()) {
        ::AllData::all_type_subclass_mutex[final_id][typ_id] = std::make_unique<std::shared_mutex>();
    }
}

static inline void
clean_all_storages() noexcept
{
    AllData::cache.clear();
    AllData::all_exist_name.clear();
    AllData::obj_attr_keys.clear();
    AllData::type_attr_dict.clear();
    AllData::type_allowed_code_map.clear();
    AllData::type_creation_locked = false;
    AllData::tmp_creation_data = nullptr;
    AllData::all_type_mutex.clear();
    AllData::type_need_call.clear();
    AllData::all_type_attr_set.clear();
    AllData::all_object_attr.clear();
    AllData::all_type_subclass_attr.clear();
    AllData::all_object_mutex.clear();
    AllData::all_type_subclass_mutex.clear();
    AllData::all_type_parent_id.clear();
    AllData::all_type_traverse.clear();
    AllData::all_type_clear.clear();
    AllData::all_register_type_weak_ref.clear();
    AllData::store_module_self.clear();
    AllData::public_attr_cache.clear();
    AllData::is_private_cache.clear();
    AllData::is_private_cache_weakref_dict.clear();
}

struct FinalObject
{
    PyObjectStorage result;
    int status = 0;
    FinalObject() noexcept = default;
    FinalObject(PyObject* result) noexcept : result(result) {}
    FinalObject(PyObjectStorage result) noexcept : result(std::move(result)) {}
    FinalObject(int status) noexcept : status(status) {}
};

static TwoStringTuple get_string_hash_tuple2(const std::string& name) noexcept;
static PyCodeObject* get_now_code() noexcept;

// --- error-aware result types for the need_analyse_type call chain ---------
// Every function in this chain can trigger a Python exception (indirectly via
// need_analyse_type -> PyObject_IsInstance / PyWeakref_GetRef). They all return
// one of these result structs. When `ok == false`, a Python exception is
// pending and the remaining fields are undefined. Callers MUST propagate an
// `ok == false` result all the way up; they must not silently swallow the
// pending exception (no PyErr_Clear on this path).
struct ParentIdsResult {
    bool ok = true;                            // false => Python exception pending
    const std::vector<uintptr_t>* ids = nullptr;
};

struct TypePrivateAttrResult {
    bool ok = true;                            // false => Python exception pending
    bool is_private = false;
};

// Result of need_remove_to_subclass(): which base declares `name` as private.
struct NeedRemoveResult {
    bool ok = true;                            // false => Python exception pending
    uintptr_t base_id = 0;                     // 0 => no base declares it private
};

// Result of classifying an attribute access.
// - type_id: the node on the MRO chain (a class) whose code is currently running
//   AND that declares the attribute as private. 0 means "no private owner".
// - allowed: false means the access must be denied (a private attribute touched
//   from outside its declaring class, or from a subclass of it).
struct AttrClassifyResult
{
    bool ok = true;                            // false => Python exception pending
    uintptr_t type_id = 0;
    bool allowed = true;
};

static ParentIdsResult type_get_parent_ids(uintptr_t type_id) noexcept;
static TypePrivateAttrResult type_private_attr(uintptr_t type, const std::string& name) noexcept;
static FinalObject type_get_final_attr(uintptr_t main_type, uintptr_t target_type, const std::string& name) noexcept;
static AttrClassifyResult attr_classify(uintptr_t typ_id, const std::string& name, PyCodeObject* code) noexcept;

static bool
is_class_code(uintptr_t typ_id, PyCodeObject* code) noexcept
{
    if (::AllData::type_allowed_code_map.find(typ_id) != ::AllData::type_allowed_code_map.end()){
        auto& code_map = ::AllData::type_allowed_code_map[typ_id];
        uintptr_t code_id = (uintptr_t)code;
        if (code_map.find(code_id) != code_map.end()){
            return true;
        }
    }
    // 2.2.1: fall back to the in-creation code-object map while a type has not
    // been fully registered yet (e.g. inside type.tp_new / a parent's
    // __init_subclass__).
    if (::AllData::type_creation_locked && ::AllData::all_type_attr_set.find(typ_id) == ::AllData::all_type_attr_set.end()){
        uintptr_t code_id = (uintptr_t)code;
        if (::AllData::tmp_creation_data &&
            ::AllData::tmp_creation_data->code_map.find(code_id) != ::AllData::tmp_creation_data->code_map.end()) {
            return true;
        }
    }
    return false;
}

static PyObject*
get_type_need_call(uintptr_t typ_id) noexcept
{
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()){
        return ::AllData::type_need_call[typ_id];
    }
    if (::AllData::type_creation_locked && ::AllData::all_type_attr_set.find(typ_id) == ::AllData::all_type_attr_set.end()) {
        if (::AllData::tmp_creation_data && ::AllData::tmp_creation_data->private_func) {
            return ::AllData::tmp_creation_data->private_func;
        }
    }
    return nullptr;
}

static bool
is_type_private(uintptr_t typ_id, const std::string& name) noexcept
{
    if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()){
        auto& attr_set = ::AllData::all_type_attr_set[typ_id];
        TwoStringTuple key = get_string_hash_tuple2(name);
        if (attr_set.find(key) != attr_set.end()){
            return true;
        }
    }
    // 2.2.1: if a type has not been registered yet (its id is absent from
    // all_type_attr_set), read from the in-creation storage instead.
    else if (::AllData::type_creation_locked  && ::AllData::all_type_attr_set.find(typ_id) == ::AllData::all_type_attr_set.end()) {
        TwoStringTuple key = get_string_hash_tuple2(name);
        if (::AllData::tmp_creation_data &&
            ::AllData::tmp_creation_data->private_attrs_set.find(key) != ::AllData::tmp_creation_data->private_attrs_set.end()){
            return true;
        }
    }
    return false;
}

static AttrClassifyResult
attr_classify(uintptr_t typ_id, const std::string& name, PyCodeObject* code) noexcept
{
    // Build the MRO chain: the actual class first, then its parents.
    std::vector<uintptr_t> chain;
    chain.push_back(typ_id);
    {
        ParentIdsResult parent_ids = type_get_parent_ids(typ_id);
        if (!parent_ids.ok) {
            return {false, 0, true};
        }
        if (parent_ids.ids) {
            chain.insert(chain.end(), parent_ids.ids->begin(), parent_ids.ids->end());
        }
    }

    // Find the class whose code is currently running (the code owner, parent first).
    uintptr_t code_class = 0;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (is_class_code(*it, code)) {
            code_class = *it;  // first hit from the base-most end
            break;
        }
    }

    if (code_class == 0) {
        // No class code was hit on the whole chain (e.g. the code belongs to a
        // module-level function or an unrelated class): if the name is private
        // to any class on the chain, deny the access.
        for (auto& node : chain) {
            if (is_type_private(node, name)) {
                return {true, 0, false};
            }
        }
        return {true, 0, true};
    }

    if (is_type_private(code_class, name)) {
        // Both conditions hold: the code is under this class AND the attribute
        // is private to this very class -> allowed, and this node of the chain
        // is the owner of the private attribute.
        return {true, code_class, true};
    }

    // The code belongs to code_class but the name is not private to it: look
    // upward (toward the base classes). If an ancestor declares the name
    // private, a subclass is trying to touch a parent's private attribute.
    // Note: a class-body definition reusing a parent's private name (a
    // "shadow", stored separately in all_type_subclass_attr[parent][child])
    // does NOT grant this class's code any access to the name.
    bool found_code_class = false;
    for (auto& node : chain) {
        if (!found_code_class) {
            if (node == code_class) {
                found_code_class = true;
            }
            continue;
        }
        if (is_type_private(node, name)) {
            return {true, 0, false};
        }
    }

    // Not private to the running class nor to any of its ancestors: treat it
    // as a normal (non-private) attribute access.
    return {true, 0, true};
}

static std::string
generate_private_attr_name(uintptr_t obj_id, const std::string& attr_name) noexcept
{
    std::string combined = std::to_string(obj_id) + "_" + compile_time + "_" + attr_name;
    std::string hash_str = picosha2::hash256_hex_string(combined);

    unsigned long long seed = std::stoul(hash_str.substr(0, 8), nullptr, 16);

    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));

    static const std::string printable_chars =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ ";

    std::uniform_int_distribution<long long> dist(0, printable_chars.size() - 1);

    auto generate_random_ascii = [&](int length) {
        std::string result;
        for(int i = 0; i < length; i++) {
            result += printable_chars[dist(rng)];
        }
        return result;
    };

    std::string part1 = generate_random_ascii(6);
    std::string part2 = generate_random_ascii(8);
    std::string part3 = generate_random_ascii(4);

    return "_" + part1 + "_" + part2 + "_" + part3;
}

static std::string
default_random_string(uintptr_t obj_id, const std::string& attr_name) noexcept
{
    AllPyobjectAttrCacheKey key(obj_id, attr_name);
    std::string result;
    {
        std::shared_lock<std::shared_mutex> lock(::AllData::cache_mutex);
        auto it = ::AllData::cache.find(key);
        if (it != ::AllData::cache.end()) {
            result = it->second;
            return result;
        } else {
            lock.unlock();
            result = generate_private_attr_name(obj_id, attr_name);
            std::string original_result = result;
            unsigned long long i = 1;
            std::unique_lock<std::shared_mutex> lock2(::AllData::cache_mutex);
            auto it = ::AllData::cache.find(key); // twice check
            if (it != ::AllData::cache.end()) {
                result = it->second;
                return result;
            }
            while (::AllData::all_exist_name.find(result) != ::AllData::all_exist_name.end()) {
                result = original_result + "_" + std::to_string(i);
                i++;
            }
            if (::AllData::obj_attr_keys.find(obj_id) == ::AllData::obj_attr_keys.end()) {
                ::AllData::obj_attr_keys[obj_id] = {};
            }
            ::AllData::obj_attr_keys[obj_id].push_back(key);
            ::AllData::cache[key] = result;
            ::AllData::all_exist_name.insert(result);
        }
    }
    return result;
}

static RestorePythonExceptionResult
custom_random_string(uintptr_t obj_id, const std::string& attr_name, PyObject* func) noexcept
{
    RestorePythonExceptionResult result;
    AllPyobjectAttrCacheKey key(obj_id, attr_name);
    {
        std::shared_lock<std::shared_mutex> lock(::AllData::cache_mutex);
        auto it = ::AllData::cache.find(key);
        if (it != ::AllData::cache.end()) {
            result.value = it->second;
            return result;
        } else {
            lock.unlock();
            PyObjectStorage python_result = PyObjectStorage::from_new_ref(
                    PyObject_CallFunction(
                    func,
                    "Ks",
                    static_cast<unsigned long long>(obj_id),
                    attr_name.c_str()
                )
            );
            if (python_result) {
                if (!PyUnicode_Check(python_result)) {
                    PyErr_Format(PyExc_TypeError, "private_func function must return a string, not '%s'", Py_TYPE(python_result)->tp_name);
                    result.ok = false;
                    result.value = "private_func function must return a string";
                    return result;
                }
                result.value = PyUnicode_AsUTF8(python_result);
                std::string original_result = result.value;
                unsigned long long i = 1;
                std::unique_lock<std::shared_mutex> lock2(::AllData::cache_mutex);
                auto it = ::AllData::cache.find(key); // twice check
                if (it != ::AllData::cache.end()) {
                    result.value = it->second;
                    return result;
                }
                while (::AllData::all_exist_name.find(result.value) != ::AllData::all_exist_name.end()) {
                    result.value = original_result + "_" + std::to_string(i);
                    i++;
                }
                if (::AllData::obj_attr_keys.find(obj_id) == ::AllData::obj_attr_keys.end()) {
                    ::AllData::obj_attr_keys[obj_id] = {};
                }
                ::AllData::obj_attr_keys[obj_id].push_back(key);
                ::AllData::cache[key] = result.value;
                ::AllData::all_exist_name.insert(result.value);
            } else {
                result.ok = false;
                result.value = "python exception while calling private function";
            }
        }
    }
    return result;
}

static void
clear_obj(uintptr_t obj_id) noexcept
{
    std::unique_lock<std::shared_mutex> lock(::AllData::cache_mutex);
    auto it = ::AllData::obj_attr_keys.find(obj_id);
    if (it != ::AllData::obj_attr_keys.end()) {
        for (auto& key: it->second) {
            std::string result = ::AllData::cache[key];
            ::AllData::all_exist_name.erase(result);
            ::AllData::cache.erase(key);
        }
        ::AllData::obj_attr_keys.erase(it);
    }
}

static const char*
get_name_from_tp_name(PyTypeObject* typ) noexcept
{
    const char* name = typ->tp_name;
    if (name == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "tp_name is NULL");
        return NULL;
    }

    const char *dot = strrchr(name, '.');
    const char *final_name = dot ? dot + 1 : name;
    return final_name;
}

static void ensure_subclass_tp(PyTypeObject* type_instance) noexcept;
static traverseproc get_need_tp_traverse(PyTypeObject* cls) noexcept;
static inquiry get_need_tp_clear(PyTypeObject* cls) noexcept;

static PyObject*
id_getattr(uintptr_t final_id, const std::string& attr_name, PyObject* obj, PyObject* typ) noexcept
{
    uintptr_t obj_id = (uintptr_t) obj;
    // Class-level fallback resolves per-subject (the instance's type): a
    // subclass's separately-stored shadow of a parent's private name is
    // visible through the per-subject walk in type_get_final_attr.
    FinalObject final_object = type_get_final_attr(final_id, (uintptr_t)typ, attr_name);
    if (final_object.status == -2) {
        return NULL;
    }

    std::string obj_private_name;
    std::string typ_private_name;
    PyObject* obj_need_call = get_type_need_call(final_id);
    if (obj_need_call) {
        auto private_name_result = custom_random_string(obj_id, attr_name, obj_need_call);
        if (!private_name_result.ok) {
            return NULL;
        }
        obj_private_name = private_name_result.value;
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
    }

    if (::AllData::all_object_attr.find(final_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return NULL;
    }
    object_create_mutex(final_id, obj_id);
    PyObject* result = NULL;
    if (final_object.status != -1) {
        result = final_object.result;
    }
    PyTypeObject* result_typ = nullptr;

    if (result) {
        result_typ = Py_TYPE(result);
        if (!result_typ) return NULL;
    }
    bool has_set = false;
    descrgetfunc get = NULL;
    if (result_typ) {
        get = result_typ->tp_descr_get;
        has_set = result_typ->tp_descr_set != NULL;
    }
    if (get && has_set) {
        PyObject* final_result = get(result, obj, typ);
        return final_result;
    }

    {
        std::shared_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[final_id][obj_id]);
        if (::AllData::all_object_attr[final_id][obj_id].find(obj_private_name) != ::AllData::all_object_attr[final_id][obj_id].end()) {
            PyObject* python_obj = ::AllData::all_object_attr[final_id][obj_id][obj_private_name];
            if (!python_obj) {
                PyErr_Format(PyExc_SystemError, "%s (%d): attribute is NULL", __FILE__, __LINE__);
                return NULL;
            }
            // if obj is a type, call result.__get__(None, obj)
            if (PyType_Check(obj)) {
                PyTypeObject* obj_type = Py_TYPE(python_obj);
                auto funcptr = obj_type->tp_descr_get;
                if (funcptr != NULL) {
                    lock.unlock();
                    return funcptr(python_obj, Py_None, obj);
                }
            }
            Py_XINCREF(python_obj);
            return python_obj;
        }
    }

    if (get) {
        PyObject* final_result = get(result, obj, typ);
        return final_result;
    }
    if (result) {
        Py_INCREF(result);
        return result;
    }
    const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
    if (type_name == NULL) {
        return NULL;
    }
    PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", type_name, attr_name.c_str());
    return NULL;
}

static PyObject*
type_getattr(uintptr_t main_type, PyObject* typ, const std::string& attr_name) noexcept
{
    uintptr_t typ_id = (uintptr_t)typ;
    FinalObject final_object = type_get_final_attr(main_type, typ_id, attr_name);
    if (final_object.status == -2) {
        return NULL;
    }
    PyObject* result = NULL;
    if (final_object.status != -1) {
        result = final_object.result;
    }
    if (result) {
        PyTypeObject* type = Py_TYPE(result);
        auto funcptr = type->tp_descr_get;
        if (funcptr) {
            PyObject* final_result = funcptr(result, Py_None, (PyObject*)type);
            return final_result;
        } else {
            Py_INCREF(result);
            return result;
        }
    }
    const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
    if (type_name == NULL) {
        return NULL;
    }
    PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%s'", type_name, attr_name.c_str());
    return NULL;
}

static int
id_setattr(uintptr_t final_id, const std::string& attr_name, PyObject* obj, PyObject* typ, PyObject* value) noexcept
{
    uintptr_t obj_id = (uintptr_t) obj;
    // Class-level fallback resolves per-subject (the instance's type).
    FinalObject final_object = type_get_final_attr(final_id, (uintptr_t)typ, attr_name);
    if (final_object.status == -2) {
        return -1;
    }

    std::string obj_private_name;
    std::string typ_private_name;
    PyObject* obj_need_call = get_type_need_call(final_id);
    if (obj_need_call) {
        auto private_name_result = custom_random_string(obj_id, attr_name, obj_need_call);
        if (!private_name_result.ok) {
            return -1;
        }
        obj_private_name = private_name_result.value;
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
    }

    if (::AllData::all_object_attr.find(final_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return -1;
    }
    object_create_mutex(final_id, obj_id);
    // first: call __set__ method
    PyObject* result = NULL;
    if (final_object.status != -1) {
        result = final_object.result;
    }
    if (result) {
        PyTypeObject* type = Py_TYPE(result);
        auto funcptr = type->tp_descr_set;
        if (funcptr) {
            if (funcptr(result, obj, value) < 0) {
                return -1;
            }
            return 0;
        }
    }

    // second: set attribute on obj
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[final_id][obj_id]);
        ::AllData::all_object_attr[final_id][obj_id][obj_private_name] = value;
    }
    return 0;
}

static int type_delattr(uintptr_t main_type, PyObject* typ, const std::string& attr_name) noexcept;

static int
type_setattr(uintptr_t main_type, PyObject* typ, const std::string& attr_name, PyObject* value) noexcept
{
    if (!value) {
        return type_delattr(main_type, typ, attr_name);
    }
    uintptr_t target_type = (uintptr_t) typ;
    std::string final_key;
    PyObject* type_need_call = get_type_need_call(target_type);
    if (type_need_call) {
        auto private_name_result = custom_random_string(target_type, attr_name, type_need_call);
        if (!private_name_result.ok) {
            return -1;
        }
        final_key = private_name_result.value;
    } else {
        final_key = default_random_string(target_type, attr_name);
    }
    if (main_type == target_type) {
        type_create_mutex(target_type);
        {
            std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[target_type]);
            ::AllData::type_attr_dict[target_type][final_key] = value;
        }
        return 0;
    } else {
        // The accessed class is a subclass that stores its own value for a
        // parent's private name separately: all_type_subclass_attr[main_type][target_type].
        type_parenttype_create_mutex(main_type, target_type);
        {
            std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[main_type][target_type]);
            ::AllData::all_type_subclass_attr[main_type][target_type][final_key] = value;
            return 0;
        }
    }
}

static int
id_delattr(uintptr_t final_id, const std::string& attr_name, PyObject* obj, PyObject* typ) noexcept
{
    uintptr_t obj_id = (uintptr_t) obj;
    // Class-level fallback resolves per-subject (the instance's type).
    FinalObject final_object = type_get_final_attr(final_id, (uintptr_t)typ, attr_name);
    if (final_object.status == -2) {
        return -1;
    }

    std::string obj_private_name;
    std::string typ_private_name;
    PyObject* obj_need_call = get_type_need_call(final_id);
    if (obj_need_call) {
        auto private_name_result = custom_random_string(obj_id, attr_name, obj_need_call);
        if (!private_name_result.ok) {
            return -1;
        }
        obj_private_name = private_name_result.value;
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
    }

    if (::AllData::all_object_attr.find(final_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return -1;
    }
    object_create_mutex(final_id, obj_id);
    // first: find attribute on type to find "__delete__"
    PyObject* result = NULL;
    if (final_object.status == 0) {
        result = final_object.result;
    }
    if (result) {
        PyTypeObject* type = Py_TYPE(result);
        auto funcptr = type->tp_descr_set;
        if (funcptr) {
            if (funcptr(result, result, NULL) < 0) {
                return -1;
            }
            return 0;
        }
    }
    // second: delete attribute on obj
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[final_id][obj_id]);
        if (::AllData::all_object_attr[final_id][obj_id].find(obj_private_name) == ::AllData::all_object_attr[final_id][obj_id].end()) {
            lock.release();
            const char* type_name;
            bool use_self_name = false;
            if (PyType_Check(obj)) {
                PyTypeObject* type = (PyTypeObject*)obj;
                type_name = get_name_from_tp_name(type);
                use_self_name = true;
            } else {
                type_name = get_name_from_tp_name((PyTypeObject*)typ);
            }
            if (type_name == NULL) {
                return -1;
            }
            if (use_self_name) {
                PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%s'", type_name, attr_name.c_str());
            } else {
                PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", type_name, attr_name.c_str());
            }
            return -1;
        }
        ::AllData::all_object_attr[final_id][obj_id].erase(obj_private_name);
    }
    return 0;
}

static int
type_delattr(uintptr_t main_type, PyObject* typ, const std::string& attr_name) noexcept
{
    uintptr_t target_type = (uintptr_t) typ;
    std::string final_key;
    PyObject* type_need_call = get_type_need_call(target_type);
    if (type_need_call) {
        auto private_name_result = custom_random_string(target_type, attr_name, type_need_call);
        if (!private_name_result.ok) {
            return -1;
        }
        final_key = private_name_result.value;
    } else {
        final_key = default_random_string(target_type, attr_name);
    }
    if (main_type == target_type) {
        type_create_mutex(target_type);
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[target_type]);
        if (::AllData::type_attr_dict[target_type].find(final_key) == ::AllData::type_attr_dict[target_type].end()) {
            const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
            if (type_name == NULL) {
                return -1;
            }
            PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%s'", type_name, attr_name.c_str());
            return -1;
        }
        ::AllData::type_attr_dict[target_type].erase(final_key);
    } else {
        type_parenttype_create_mutex(main_type, target_type);
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[main_type][target_type]);
        if (::AllData::all_type_subclass_attr[main_type][target_type].find(final_key) == ::AllData::all_type_subclass_attr[main_type][target_type].end()) {
            const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
            if (type_name == NULL) {
                return -1;
            }
            PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%s'", type_name, attr_name.c_str());
            return -1;
        }
        ::AllData::all_type_subclass_attr[main_type][target_type].erase(final_key);
    }
    return 0;
}

// list proxy
// privatewrap.funcs return this type after 2.1.19

typedef struct {
    PyObject_HEAD
    PyObject *list;
} listproxy;

static bool
listproxy_check(listproxy *obj) noexcept
{
    if (!obj->list) {
        PyErr_SetString(PyExc_RuntimeError, "listproxy object has no list");
        return false;
    }
    if (!PyList_CheckExact(obj->list)) {
        PyErr_Format(PyExc_TypeError, "listproxy object can only proxy list, not '%s'", Py_TYPE(obj->list)->tp_name);
        return false;
    }
    return true;
}

static PyObject*
listproxy_copy(PyObject* self, PyObject* /*args*/) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return NULL;
    }
    return PySequence_List(obj->list);
}

static PyObject*
listproxy_count(PyObject* self, PyObject* value) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return NULL;
    }
    Py_ssize_t m =  PySequence_Count(obj->list, value);
    if (m < 0) {
        return NULL;
    }
    PyObject* result = PyLong_FromSsize_t(m);
    return result;
}

static PyObject*
listproxy_index(PyObject* self, PyObject* args) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return NULL;
    }
    PyObjectStorage method = PyObjectStorage::from_new_ref(PyObject_GetAttrString(obj->list, "index"));
    if (!method) {
        return NULL;
    }
    PyObject* result = PyObject_Call(method.get(), args, NULL);
    return result;
}

static Py_ssize_t
listproxy_length(PyObject* self) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return -1;
    }
    return PyList_Size(obj->list);
}

static PyObject*
listproxy_iter(PyObject* self) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return NULL;
    }
    return PyObject_GetIter(obj->list);
}

static PyObject*
listproxy_item(PyObject* self, Py_ssize_t i) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return NULL;
    }
    Py_ssize_t size = PyList_Size(obj->list);
    if (i < 0) {
        i += size;
    }
    if (i < 0 || i >= size) {
        PyErr_SetString(PyExc_IndexError, "listproxy index out of range");
        return NULL;
    }
    PyObject* item = PyList_GetItem(obj->list, i);
    if (!item) {
        return NULL;
    }
    Py_INCREF(item);
    return item;
}

static PyObject*
listproxy_subscript(PyObject* self, PyObject* key) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return NULL;
    }
    return PyObject_GetItem(obj->list, key);
}

static int
listproxy_contains(PyObject* self, PyObject* value) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return -1;
    }
    return PySequence_Contains(obj->list, value);
}

static PyObject*
listproxy_repr(PyObject* self) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return NULL;
    }
    PyObjectStorage inner = PyObjectStorage::from_new_ref(PyObject_Repr(obj->list));
    if (!inner) {
        return NULL;
    }
    PyObject* result = PyUnicode_FromFormat("listproxy(%U)", inner.get());
    return result;
}

static PyObject*
listproxy_richcompare(PyObject* self, PyObject* other, int op) noexcept
{
    listproxy* obj = (listproxy*)self;
    if (!listproxy_check(obj)) {
        return NULL;
    }

    // If the counterpart is a genuine Python list, compare the underlying
    // list directly. Otherwise copy the counterpart into a real list first
    // (so that listproxy == listproxy and listproxy == tuple behave like a
    // list-to-list comparison), then compare.
    PyObjectStorage self_list = obj->list;
    if (!PyList_CheckExact(other)) {
        self_list = PyObjectStorage::from_new_ref(PySequence_List(obj->list));
        if (!self_list) {
            PyErr_Clear();
            Py_RETURN_NOTIMPLEMENTED;
        }
    }

    PyObject* result = PyObject_RichCompare(self_list, other, op);
    return result;
}

static void
listproxy_dealloc(listproxy* self) noexcept
{
    Py_XDECREF(self->list);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PySequenceMethods listproxy_as_sequence = {
    listproxy_length,       // sq_length
    0,                      // sq_concat
    0,                      // sq_repeat
    listproxy_item,         // sq_item
    0,                      // was_sq_slice
    0,                      // sq_ass_item
    0,                      // was_sq_ass_slice
    listproxy_contains,     // sq_contains
    0,                      // sq_inplace_concat
    0,                      // sq_inplace_repeat
};

static PyMappingMethods listproxy_as_mapping = {
    listproxy_length,       // mp_length
    listproxy_subscript,    // mp_subscript
    0,                      // mp_ass_subscript
};

static PyMethodDef listproxy_methods[] = {
    {"copy", listproxy_copy, METH_NOARGS, "Return a copy of the proxied list."},
    {"count", listproxy_count, METH_O, "Return number of occurrences of value."},
    {"index", listproxy_index, METH_VARARGS, "Return first index of value."},
    {NULL, NULL, 0, NULL}
};

static int
listproxy_traverse(PyObject* self, visitproc visit, void* arg) noexcept
{
    listproxy* obj = (listproxy*)self;
    Py_VISIT(obj->list);
    return 0;
}

static int
listproxy_clear(PyObject* self) noexcept
{
    listproxy* obj = (listproxy*)self;
    Py_CLEAR(obj->list);
    return 0;
}

static PyTypeObject listproxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute._listproxy",          // tp_name
    sizeof(listproxy),                       // tp_basicsize
    0,                                       // tp_itemsize
    (destructor)listproxy_dealloc,           // tp_dealloc
    0,                                       // tp_print
    0,                                       // tp_getattr
    0,                                       // tp_setattr
    0,                                       // tp_reserved
    listproxy_repr,                          // tp_repr
    0,                                       // tp_as_number
    &listproxy_as_sequence,                  // tp_as_sequence
    &listproxy_as_mapping,                   // tp_as_mapping
    0,                                       // tp_hash
    0,                                       // tp_call
    0,                                       // tp_str
    0,                                       // tp_getattro
    0,                                       // tp_setattro
    0,                                       // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    "A read-only proxy of a list.",          // tp_doc
    listproxy_traverse,                      // tp_traverse
    listproxy_clear,                         // tp_clear
    listproxy_richcompare,                   // tp_richcompare
    0,                                       // tp_weaklistoffset
    listproxy_iter,                          // tp_iter
    0,                                       // tp_iternext
    listproxy_methods,                       // tp_methods
};

static PyObject*
listproxy_new(PyObject* list) noexcept
{
    listproxy* obj = PyObject_GC_New(listproxy, &listproxyType);
    if (!obj) {
        return NULL;
    }
    Py_INCREF(list);
    obj->list = list;
    PyObject_GC_Track(obj);
    return (PyObject*)obj;
}

// ================================================================
// _PrivateWrap
// ================================================================
typedef struct {
    PyObject_HEAD
    PyObject *result;
    PyObject *func_list;
} PrivateWrapObject;

static PrivateWrapObject* PrivateWrap_New(PyObject *decorator, PyObject *func, PyObject *list) noexcept;
static void PrivateWrap_dealloc(PrivateWrapObject *self) noexcept;
static PyObject* PrivateWrap_call(PrivateWrapObject *self, PyObject *args, PyObject *kw) noexcept;
static PyObject* PrivateWrap_SetName(PyObject* self, PyObject* args) noexcept;

static PyMemberDef PrivateWrap_members[] = {
    {"result", T_OBJECT, offsetof(PrivateWrapObject, result), READONLY, "The result object"},
    {"__wrapped__", T_OBJECT, offsetof(PrivateWrapObject, result), READONLY, "The result object"},
    {NULL}
};

static PyMethodDef PrivateWrap_methods[] = {
    {"__set_name__", PrivateWrap_SetName, METH_VARARGS, "Set the name of the wrapped object."},
    {NULL}
};

static PyObject*
PrivateWrap_SetName(PyObject* self, PyObject* args) noexcept
{
    PrivateWrapObject* obj = (PrivateWrapObject*)self;
    if (PyObject_HasAttrString(obj->result, "__set_name__")) {
        // call result.__set_name__(*args)
        PyObjectStorage object_set_name = PyObjectStorage::from_new_ref(PyObject_GetAttrString(obj->result, "__set_name__"));
        if (!object_set_name) {
            return NULL;
        }
        PyObjectStorage args_set_name = PyObjectStorage::from_new_ref(PyObject_CallObject(object_set_name, args));
        if (!args_set_name) {
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *
PrivateWrap_funcs(PyObject *obj, void* /*closure*/) noexcept
{
    if (!obj) {
        Py_RETURN_NONE;
    }

    return listproxy_new(((PrivateWrapObject*)obj)->func_list);
}

static PyObject*
PrivateWrap_doc(PyObject *obj, void* /*closure*/) noexcept
{
    if (!obj) {
        return PyUnicode_InternFromString("PrivateWrap");
    }
    PyObject* doc = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__doc__");
    if (!doc) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    return doc;
}

static PyObject*
PrivateWrap_module(PyObject *obj, void* /*closure*/) noexcept
{
    if (!obj) {
        return PyUnicode_InternFromString("private_attribute");
    }
    PyObject* module = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__module__");
    if (!module){
        PyErr_Clear();
        return PyUnicode_InternFromString("private_attribute");
    }
    return module;
}

static PyObject*
PrivateWrap_name(PyObject* obj, void* /*closure*/) noexcept
{
    if (!obj) {
        return PyUnicode_InternFromString("_PrivateWrap");
    }
    PyObject* name = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__name__");
    if (!name) {
        PyErr_Clear();
        return PyUnicode_InternFromString("_PrivateWrap");
    }
    return name;
}

static PyObject*
PrivateWrap_qualname(PyObject* obj, void* /*closure*/) noexcept
{
    if (!obj) {
        return PyUnicode_InternFromString("private_attribute._PrivateWrap");
    }
    PyObject* qualname = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__qualname__");
    if (!qualname) {
        PyErr_Clear();
        return PyUnicode_InternFromString("private_attribute._PrivateWrap");
    }
    return qualname;
}

// __annotate__
static PyObject*
PrivateWrap_annotate(PyObject* obj, void* /*closure*/) noexcept
{
    if (!obj) {
        Py_RETURN_NONE;
    }
    PyObject* annotate = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__annotate__");
    if (!annotate) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    return annotate;
}

// __type_params__
static PyObject*
PrivateWrap_type_params(PyObject* obj, void* /*closure*/) noexcept
{
    if (!obj) {
        Py_RETURN_NONE;
    }
    PyObject* type_params = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__type_params__");
    if (!type_params) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    return type_params;
}

static PyObject*
PrivateWrap_isabstractmethod(PyObject* obj, void* /*closure*/) noexcept
{
    if (!obj) {
        Py_RETURN_FALSE;
    }
    PyObject* isabstractmethod = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__isabstractmethod__");
    if (!isabstractmethod) {
        PyErr_Clear();
        Py_RETURN_FALSE;
    }
    return isabstractmethod;
}

static int
PrivateWrap_set_isabstractmethod(PyObject* obj, PyObject* value, void* /*closure*/) noexcept
{
    if (!obj) {
        return 0;
    }
    return PyObject_SetAttrString(((PrivateWrapObject*)obj)->result, "__isabstractmethod__", value);
}

static const char* PrivateWrap_funcs_doc = "the original functions";

static PyGetSetDef PrivateWrap_getset[] = {
    {"funcs", (getter)PrivateWrap_funcs, NULL, PrivateWrap_funcs_doc, NULL},
    {"__doc__", (getter)PrivateWrap_doc, NULL, NULL, NULL},
    {"__module__", (getter)PrivateWrap_module, NULL, NULL, NULL},
    {"__name__", (getter)PrivateWrap_name, NULL, NULL, NULL},
    {"__qualname__", (getter)PrivateWrap_qualname, NULL, NULL, NULL},
    {"__annotate__", (getter)PrivateWrap_annotate, NULL, NULL, NULL},
    {"__type_params__", (getter)PrivateWrap_type_params, NULL, NULL, NULL},
    {"__isabstractmethod__", (getter)PrivateWrap_isabstractmethod, (setter)PrivateWrap_set_isabstractmethod, NULL, NULL},
    {NULL}
};

#define PrivateWrap_getattro_dunder(name) do {\
if (strcmp(name_str, "__" #name "__") == 0) {\
    return PrivateWrap_##name(obj, NULL);\
}\
} while(0)

static PyObject *
PrivateWrap_getattro(PyObject *obj, PyObject *name) noexcept
{
    PrivateWrapObject *self = (PrivateWrapObject *)obj;
    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) {
        PyErr_SetString(PyExc_TypeError, "attribute name must be a string");
        return NULL;
    }
    if (strcmp(name_str, "__wrapped__") == 0 || strcmp(name_str, "result") == 0) {
        if (!self->result) {
            PyErr_SetString(PyExc_AttributeError, "attribute 'result' is invalid");
            return NULL;
        }
        Py_INCREF(self->result);
        return self->result;
    }
    if (strcmp(name_str, "funcs") == 0) {
        return PrivateWrap_funcs(obj, NULL);
    }
    PrivateWrap_getattro_dunder(doc);
    PrivateWrap_getattro_dunder(module);
    PrivateWrap_getattro_dunder(name);
    PrivateWrap_getattro_dunder(qualname);
    PrivateWrap_getattro_dunder(annotate);
    PrivateWrap_getattro_dunder(type_params);
    PrivateWrap_getattro_dunder(isabstractmethod);
    return PyObject_GetAttr(self->result, name);
}

static PyObject*
PrivateWrap_descrget(PyObject* obj, PyObject* name, PyObject* cls) noexcept
{
    PyObject* result = ((PrivateWrapObject*)obj)->result;
    auto funcptr = Py_TYPE(result)->tp_descr_get;
    if (funcptr) {
        return funcptr(result, name, cls);
    } else {
        Py_INCREF(result);
        return result;
    }
}

static int
PrivateWrap_descrset(PyObject* obj, PyObject* name, PyObject* value) noexcept
{
    PyObject* result = ((PrivateWrapObject*)obj)->result;
    auto funcptr = Py_TYPE(result)->tp_descr_set;
    if (funcptr) {
        return funcptr(result, name, value);
    } else {
        PyErr_SetString(PyExc_AttributeError, "attribute is not settable");
        return -1;
    }
}

static int
PrivateWrap_traverse(PyObject* obj, visitproc visit, void* arg) noexcept
{
    PrivateWrapObject* self = (PrivateWrapObject*)obj;
    if (self->result) {
        Py_VISIT(self->result);
    }
    if (self->func_list) {
        Py_VISIT(self->func_list);
    }
    return 0;
}

static int
PrivateWrap_clear(PyObject* obj) noexcept
{
    PrivateWrapObject* self = (PrivateWrapObject*)obj;
    if (self->result) {
        Py_CLEAR(self->result);
    }
    if (self->func_list) {
        Py_CLEAR(self->func_list);
    }
    return 0;
}

static PyTypeObject PrivateWrapType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute._PrivateWrap",          // tp_name
    sizeof(PrivateWrapObject),                 // tp_basicsize
    0,                                         // tp_itemsize
    (destructor)PrivateWrap_dealloc,           // tp_dealloc
    0,                                         // tp_print
    0,                                         // tp_getattr
    0,                                         // tp_setattr
    0,                                         // tp_reserved
    0,                                         // tp_repr
    0,                                         // tp_as_number
    0,                                         // tp_as_sequence
    0,                                         // tp_as_mapping
    0,                                         // tp_hash
    (ternaryfunc)PrivateWrap_call,             // tp_call
    0,                                         // tp_str
    PrivateWrap_getattro,                      // tp_getattro
    0,                                         // tp_setattro
    0,                                         // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,   // tp_flags
    "private_attribute._PrivateWrap",          // tp_doc
    PrivateWrap_traverse,                      // tp_traverse
    PrivateWrap_clear,                         // tp_clear
    0,                                         // tp_richcompare
    0,                                         // tp_weaklistoffset
    0,                                         // tp_iter
    0,                                         // tp_iternext
    PrivateWrap_methods,                       // tp_methods
    PrivateWrap_members,                       // tp_members
    PrivateWrap_getset,                        // tp_getset
    0,                                         // tp_base
    0,                                         // tp_dict
    PrivateWrap_descrget,                      // tp_descr_get
    PrivateWrap_descrset,                      // tp_descr_set
};

static PrivateWrapObject*
PrivateWrap_New(PyObject *decorator, PyObject *func, PyObject *list) noexcept
{
    PyObject *wrapped = PyObject_CallFunctionObjArgs(decorator, func, NULL);
    if (!wrapped) {
        return NULL;
    }
    if (PyObject_TypeCheck(wrapped, &PrivateWrapType)) {
        PyErr_SetString(PyExc_TypeError, "decorator returned a '_PrivateWrap' object which is not allowed");
        Py_DECREF(wrapped);
        return NULL;
    }

    PrivateWrapObject *self =
        PyObject_GC_New(PrivateWrapObject, &PrivateWrapType);
    if (!self) {
        Py_DECREF(wrapped);
        return NULL;
    }
    self->func_list = list;
    Py_INCREF(list);

    self->result = wrapped;
    PyObject_GC_Track(self);

    return self;
}

static void
PrivateWrap_dealloc(PrivateWrapObject *self) noexcept
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->result);
    Py_XDECREF(self->func_list);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject*
PrivateWrap_call(PrivateWrapObject *self, PyObject *args, PyObject *kw) noexcept
{
    return PyObject_Call(self->result, args, kw);
}

// ================================================================
// PrivateWrapProxy
// ================================================================
typedef struct {
    PyObject_HEAD
    PyObject *decorator;  // _decorator
    PyObject *func_list;  // _func_list
} PrivateWrapProxyObject;

static PyObject*
PrivateWrapProxy_call(PrivateWrapProxyObject *self, PyObject *args, PyObject* /* kwgs */) noexcept
{
    PyObject *func;
    if (!PyArg_ParseTuple(args, "O:private_attribute.PrivateWrapProxy.__call__", &func)) return NULL;
    if(PyObject_TypeCheck(func, &PrivateWrapType)) {
        PyObjectStorage final_func_list = PyObjectStorage::from_new_ref(
            PySequence_Concat(((PrivateWrapObject*)func)->func_list, self->func_list));
        return (PyObject*)PrivateWrap_New(
            self->decorator,
            ((PrivateWrapObject*)func)->result,
            final_func_list
        );
    }

    PyObject *new_list = PyList_New(0);
    PyList_Append(new_list, func);

    PyObjectStorage combined =
        PyObjectStorage::from_new_ref(PySequence_Concat(new_list, self->func_list));

    return (PyObject*)PrivateWrap_New(
        self->decorator,
        func,
        combined
    );
}

static void PrivateWrapProxy_dealloc(PrivateWrapProxyObject *self) noexcept;
static PyObject* PrivateWrapProxy_New(PyTypeObject *type, PyObject *args, PyObject *kwds) noexcept;
static const char* PrivateWrapProxy_doc = R"(
PrivateWrapProxy is a proxy for private attributes.
Usage:
```
from private_attribute import PrivateWrapProxy, PrivateAttrBase

class MyClass(PrivateAttrBase):
    __private_attrs__ = ()
    @PrivateWrapProxy(decorator)
    def my_method(self): ...

    @PrivateWrapProxy(decorator)
    def my_method2(self): ...
```
It returned a '_PrivateWrap' object.

If you need to decorate more function, use like this:
```
from private_attribute import PrivateWrapProxy, PrivateAttrBase

class MyClass(PrivateAttrBase):
    __private_attrs__ = ()
    @PrivateWrapProxy(decorator)
    def my_method(self): ...

    @PrivateWrapProxy(my_method.some_decorator, my_method)
    def my_method(self): ...
```
)";

static int
PrivateWrapProxy_traverse(PrivateWrapProxyObject *self, visitproc visit, void *arg) noexcept
{
    Py_VISIT(self->decorator);
    Py_VISIT(self->func_list);
    return 0;
}

static int
PrivateWrapProxy_clear(PrivateWrapProxyObject *self) noexcept
{
    Py_CLEAR(self->decorator);
    Py_CLEAR(self->func_list);
    return 0;
}

static PyTypeObject PrivateWrapProxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute.PrivateWrapProxy",   // tp_name
    sizeof(PrivateWrapProxyObject),         // tp_basicsize
    0,                                      // tp_itemsize
    (destructor)PrivateWrapProxy_dealloc,   // tp_dealloc
    0,                                      // tp_print
    0,                                      // tp_getattr
    0,                                      // tp_setattr
    0,                                      // tp_reserved
    0,                                      // tp_repr
    0,                                      // tp_as_number
    0,                                      // tp_as_sequence
    0,                                      // tp_as_mapping
    0,                                      // tp_hash
    (ternaryfunc)PrivateWrapProxy_call,     // tp_call
    0,                                      // tp_str
    0,                                      // tp_getattro
    0,                                      // tp_setattro
    0,                                      // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,// tp_flags
    PrivateWrapProxy_doc,                   // tp_doc
    (traverseproc)PrivateWrapProxy_traverse,// tp_traverse
    (inquiry)PrivateWrapProxy_clear,        // tp_clear
    0,                                      // tp_richcompare
    0,                                      // tp_weaklistoffset
    0,                                      // tp_iter
    0,                                      // tp_iternext
    0,                                      // tp_methods
    0,                                      // tp_members
    0,                                      // tp_getset
    0,                                      // tp_base
    0,                                      // tp_dict
    0,                                      // tp_descr_get
    0,                                      // tp_descr_set
    0,                                      // tp_dictoffset
    0,                                      // tp_init
    0,                                      // tp_alloc
    PrivateWrapProxy_New,                   // tp_new
};

static PyObject*
PrivateWrapProxy_New(PyTypeObject *type, PyObject *args, PyObject* /*kwds*/) noexcept
{
    PrivateWrapProxyObject *self;
    PyObject *decorator;
    PyObject *orig = NULL;

    if (!PyArg_ParseTuple(args, "O|O:private_attribute.PrivateWrapProxy", &decorator, &orig))
        return NULL;

    self = (PrivateWrapProxyObject*)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;

    Py_INCREF(decorator);
    self->decorator = decorator;

    if (orig && PyObject_TypeCheck(orig, &PrivateWrapType)) {
        self->func_list = ((PrivateWrapObject*)orig)->func_list;
        Py_INCREF(self->func_list);
    }
    else {
        self->func_list = PyList_New(0);
        if (self->func_list == NULL) {
            Py_DECREF(self);
            return NULL;
        }
    }

    return (PyObject*)self;
}

static void
PrivateWrapProxy_dealloc(PrivateWrapProxyObject *self) noexcept
{
    Py_XDECREF(self->decorator);
    Py_XDECREF(self->func_list);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

// ================================================================
// PrivateSuper / PrivateClassSuper
// ================================================================
// Explicit, cross-class access to a parent class's class-level private
// attribute (stored in all_type_subclass_attr). These are NOT for instance
// attribute cross-type access — they only read the class-level shadow that a
// parent type stores for a subclass.
//
//   PrivateSuper(A, B, C, obj).name
//       A : the type whose code is currently running (the code owner).
//       B : a parent type of A whose class-level private attribute we access.
//       C : the class the private variable was defined for; it must be B, A,
//           or a class that is both a subclass of B and a parent of A.
//       obj : an instance of A.
//       Resolves `name` in all_type_subclass_attr[B][C] and calls its
//       __get__(obj, A).
//
//   PrivateClassSuper(A, B, C, cls).name
//       Same as PrivateSuper, except cls is a subclass of A and the resolved
//       descriptor's __get__(None, cls) is called (classmethod flavour).
//
// The A/B/C relationships and the obj/cls argument are validated at
// construction time; the code-ownership check (the running code must belong
// to A) is enforced at attribute access time.

typedef struct {
    PyObject_HEAD
    PyObject* A;   // current code owner type
    PyObject* B;   // parent type whose private attrs are accessed
    PyObject* C;   // the class the private variable was defined for
    PyObject* obj; // instance of A (PrivateSuper) or subclass of A (PrivateClassSuper)
} PrivateSuperObject;

static bool private_super_validate(PrivateSuperObject* self, bool is_class) noexcept;
static PyObject* private_super_getattro(PyObject* self, PyObject* name, bool is_class) noexcept;
static PyObject* PrivateSuper_getattro(PyObject* self, PyObject* name) noexcept;
static PyObject* PrivateClassSuper_getattro(PyObject* self, PyObject* name) noexcept;
static int PrivateSuper_traverse(PrivateSuperObject* self, visitproc visit, void* arg) noexcept;
static int PrivateSuper_clear(PrivateSuperObject* self) noexcept;
static void PrivateSuper_dealloc(PrivateSuperObject* self) noexcept;
static PyObject* PrivateSuper_New(PyTypeObject* type, PyObject* args, PyObject* kwds) noexcept;

static const char* PrivateSuper_doc =
"Explicit cross-class access to a parent class's class-level private attribute.\n"
"Usage: PrivateSuper(A, B, C, obj).name\n"
"A, B and C must all be private-attribute types; A is the running code's type,\n"
"B is a parent of A, C is B, A, or both a subclass of B and a parent of A, and\n"
"obj is an instance of A. The attribute is resolved from B's class-level storage\n"
"for C and its __get__(obj, A) is invoked.";

static const char* PrivateClassSuper_doc =
"Classmethod flavour of PrivateSuper.\n"
"Usage: PrivateClassSuper(A, B, C, cls).name\n"
"A, B and C are as in PrivateSuper; cls is a subclass of A. The attribute is\n"
"resolved from B's class-level storage for C and its __get__(None, cls) is invoked.";

static PyTypeObject PrivateSuperType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute.PrivateSuper",       // tp_name
    sizeof(PrivateSuperObject),             // tp_basicsize
    0,                                      // tp_itemsize
    (destructor)PrivateSuper_dealloc,       // tp_dealloc
    0,                                      // tp_print
    0,                                      // tp_getattr
    0,                                      // tp_setattr
    0,                                      // tp_reserved
    0,                                      // tp_repr
    0,                                      // tp_as_number
    0,                                      // tp_as_sequence
    0,                                      // tp_as_mapping
    0,                                      // tp_hash
    0,                                      // tp_call
    0,                                      // tp_str
    (getattrofunc)PrivateSuper_getattro,    // tp_getattro
    0,                                      // tp_setattro
    0,                                      // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,// tp_flags
    PrivateSuper_doc,                       // tp_doc
    (traverseproc)PrivateSuper_traverse,    // tp_traverse
    (inquiry)PrivateSuper_clear,            // tp_clear
    0,                                      // tp_richcompare
    0,                                      // tp_weaklistoffset
    0,                                      // tp_iter
    0,                                      // tp_iternext
    0,                                      // tp_methods
    0,                                      // tp_members
    0,                                      // tp_getset
    0,                                      // tp_base
    0,                                      // tp_dict
    0,                                      // tp_descr_get
    0,                                      // tp_descr_set
    0,                                      // tp_dictoffset
    0,                                      // tp_init
    0,                                      // tp_alloc
    (newfunc)PrivateSuper_New,              // tp_new
};

static PyTypeObject PrivateClassSuperType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute.PrivateClassSuper",  // tp_name
    sizeof(PrivateSuperObject),             // tp_basicsize
    0,                                      // tp_itemsize
    (destructor)PrivateSuper_dealloc,       // tp_dealloc
    0,                                      // tp_print
    0,                                      // tp_getattr
    0,                                      // tp_setattr
    0,                                      // tp_reserved
    0,                                      // tp_repr
    0,                                      // tp_as_number
    0,                                      // tp_as_sequence
    0,                                      // tp_as_mapping
    0,                                      // tp_hash
    0,                                      // tp_call
    0,                                      // tp_str
    (getattrofunc)PrivateClassSuper_getattro, // tp_getattro
    0,                                      // tp_setattro
    0,                                      // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,// tp_flags
    PrivateClassSuper_doc,                  // tp_doc
    (traverseproc)PrivateSuper_traverse,    // tp_traverse
    (inquiry)PrivateSuper_clear,            // tp_clear
    0,                                      // tp_richcompare
    0,                                      // tp_weaklistoffset
    0,                                      // tp_iter
    0,                                      // tp_iternext
    0,                                      // tp_methods
    0,                                      // tp_members
    0,                                      // tp_getset
    0,                                      // tp_base
    0,                                      // tp_dict
    0,                                      // tp_descr_get
    0,                                      // tp_descr_set
    0,                                      // tp_dictoffset
    0,                                      // tp_init
    0,                                      // tp_alloc
    (newfunc)PrivateSuper_New,              // tp_new
};

// Index of `id` in `mro` (a private-attribute ancestor list in MRO order,
// nearest first), or -1 if absent.
static Py_ssize_t
mro_index_of(const std::vector<uintptr_t>& mro, uintptr_t id) noexcept
{
    for (Py_ssize_t i = 0; i < mro.size(); ++i) {
        if (mro[i] == id) {
            return i;
        }
    }
    return -1;
}

static bool
private_super_validate(PrivateSuperObject* self, bool is_class) noexcept
{
    // A, B, C must all be types.
    if (!PyType_Check(self->A) || !PyType_Check(self->B) || !PyType_Check(self->C)) {
        PyErr_SetString(PyExc_TypeError, "A, B and C must be types");
        return false;
    }
    // A, B, C must all need private-attribute analysis.
    for (PyObject* t : {self->A, self->B, self->C}) {
        AnalyseStatus st = need_analyse_type(t);
        if (st == AnalyseStatus::Error) {
            return false;
        }
        if (st != AnalyseStatus::Yes) {
            PyErr_SetString(PyExc_ValueError,
                "A, B and C must all be private-attribute types (instances of "
                "PrivateAttrType or a registered metaclass)");
            return false;
        }
    }
    // B must be a parent of A. Instead of PyObject_IsSubclass(A, B), use A's
    // stored private-attribute ancestor MRO (all_type_parent_id[A], nearest
    // first) and check membership by id.
    if (self->A == self->B) {
        PyErr_SetString(PyExc_ValueError, "B must be a parent type of A, not A itself");
        return false;
    }
    uintptr_t a_id = (uintptr_t)self->A;
    uintptr_t b_id = (uintptr_t)self->B;
    uintptr_t c_id = (uintptr_t)self->C;
    ParentIdsResult a_parents = type_get_parent_ids(a_id);
    if (!a_parents.ok) {
        return false;
    }
    Py_ssize_t b_idx = mro_index_of(*a_parents.ids, b_id);
    if (b_idx < 0) {
        PyErr_SetString(PyExc_ValueError, "B must be a parent type of A");
        return false;
    }
    // C must be B, A, or both a subclass of B and a parent of A. Both
    // relationships are answered from the stored MRO id lists (no
    // PyObject_IsSubclass): C is a parent of A iff C is in A's ancestor list,
    // and C is a subclass of B iff B is in C's ancestor list (B sits after C
    // on C's inheritance chain).
    bool c_ok = (c_id == b_id) || (c_id == a_id);
    if (!c_ok) {
        if (mro_index_of(*a_parents.ids, c_id) >= 0) {
            ParentIdsResult c_parents = type_get_parent_ids(c_id);
            if (!c_parents.ok) {
                return false;
            }
            c_ok = (mro_index_of(*c_parents.ids, b_id) >= 0);
        }
    }
    if (!c_ok) {
        PyErr_SetString(PyExc_ValueError,
            "C must be B, A, or a class that is both a subclass of B and a parent of A");
        return false;
    }
    // obj must be an instance of A; cls must be a subclass of A.
    if (is_class) {
        if (!PyType_Check(self->obj)) {
            PyErr_SetString(PyExc_TypeError, "cls must be a type");
            return false;
        }
        uintptr_t obj_id = (uintptr_t)self->obj;
        // A type is a subclass of itself; otherwise cls is a subclass of A iff
        // A appears in cls's private-attribute ancestor MRO.
        if (obj_id != a_id) {
            ParentIdsResult obj_parents = type_get_parent_ids(obj_id);
            if (!obj_parents.ok) {
                return false;
            }
            if (mro_index_of(*obj_parents.ids, a_id) < 0) {
                PyErr_SetString(PyExc_TypeError, "cls must be a subclass of A");
                return false;
            }
        }
    } else {
        int ok = PyObject_IsInstance(self->obj, self->A);
        if (ok < 0) {
            return false;
        }
        if (!ok) {
            PyErr_SetString(PyExc_TypeError, "obj must be an instance of A");
            return false;
        }
    }
    return true;
}

static PyObject*
private_super_getattro(PyObject* self, PyObject* name, bool is_class) noexcept
{
    PrivateSuperObject* s = (PrivateSuperObject*)self;
    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "attribute name must be a string");
        return NULL;
    }
    const char* name_cstr = PyUnicode_AsUTF8(name);
    if (!name_cstr) {
        return NULL;
    }
    std::string attr_name(name_cstr);

    // The running code must belong to A.
    PyObjectStorage now_code = PyObjectStorage::from_new_ref(get_now_code());
    if (!now_code) {
        PyErr_SetString(PyExc_AttributeError, "cannot use 'PrivateSuper' or 'PrivateClassSuper' outside the class");
        return NULL;
    }
    if (!is_class_code((uintptr_t)s->A, (PyCodeObject*)now_code.get())) {
        PyErr_SetString(PyExc_AttributeError, "cannot use 'PrivateSuper' or 'PrivateClassSuper' outside the class");
        return NULL;
    }

    // Only private attributes of B (the second type) can be read through
    // PrivateSuper/PrivateClassSuper; reject any other name early, before any
    // storage-key work.
    TypePrivateAttrResult pa = type_private_attr((uintptr_t)s->B, attr_name);
    if (!pa.ok) {
        return NULL;
    }
    if (!pa.is_private) {
        const char* tn = get_name_from_tp_name((PyTypeObject*)s->B);
        if (tn == NULL) {
            return NULL;
        }
        PyErr_Format(PyExc_AttributeError, "'%s' has no private attribute '%s'", tn, attr_name.c_str());
        return NULL;
    }

    // Resolve the storage key for C and look up the class-level storage.
    //   C == B -> B's own class body storage (type_attr_dict[B])
    //   C != B -> B's shadow for C (all_type_subclass_attr[B][C])
    uintptr_t main_type = (uintptr_t)s->B;
    uintptr_t target_type = (uintptr_t)s->C;
    std::string key;
    PyObject* need_call = get_type_need_call(target_type);
    if (need_call) {
        auto r = custom_random_string(target_type, attr_name, need_call);
        if (!r.ok) {
            return NULL;
        }
        key = r.value;
    } else {
        key = default_random_string(target_type, attr_name);
    }

    PyObjectStorage value;
    if (main_type == target_type) {
        type_create_mutex(target_type);
        std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[target_type]);
        auto& m = ::AllData::type_attr_dict[target_type];
        auto it = m.find(key);
        if (it == m.end()) {
            const char* tn = get_name_from_tp_name((PyTypeObject*)s->C);
            if (tn == NULL) {
                return NULL;
            }
            PyErr_Format(PyExc_AttributeError, "'%s' has no class attribute '%s'", tn, attr_name.c_str());
            return NULL;
        }
        value = it->second;
    } else {
        type_parenttype_create_mutex(main_type, target_type);
        std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[main_type][target_type]);
        auto& m = ::AllData::all_type_subclass_attr[main_type][target_type];
        auto it = m.find(key);
        if (it == m.end()) {
            const char* tn = get_name_from_tp_name((PyTypeObject*)s->C);
            if (tn == NULL) {
                return NULL;
            }
            PyErr_Format(PyExc_AttributeError, "'%s' has no class attribute '%s'", tn, attr_name.c_str());
            return NULL;
        }
        value = it->second;
    }
    if (!value) {
        PyErr_Format(PyExc_SystemError, "%s (%d): attribute is NULL", __FILE__, __LINE__);
        return NULL;
    }

    // Invoke the descriptor's __get__: PrivateSuper -> __get__(obj, A);
    // PrivateClassSuper -> __get__(None, cls).
    PyObject* owner = is_class ? s->obj : s->A;
    PyObject* inst = is_class ? Py_None : s->obj;
    descrgetfunc get = Py_TYPE(value)->tp_descr_get;
    if (get) {
        PyObject* r = get(value, inst, owner);
        return r;
    }
    Py_INCREF(value);
    return value;
}

static PyObject*
PrivateSuper_getattro(PyObject* self, PyObject* name) noexcept
{
    return private_super_getattro(self, name, false);
}

static PyObject*
PrivateClassSuper_getattro(PyObject* self, PyObject* name) noexcept
{
    return private_super_getattro(self, name, true);
}

static int
PrivateSuper_traverse(PrivateSuperObject* self, visitproc visit, void* arg) noexcept
{
    Py_VISIT(self->A);
    Py_VISIT(self->B);
    Py_VISIT(self->C);
    Py_VISIT(self->obj);
    return 0;
}

static int
PrivateSuper_clear(PrivateSuperObject* self) noexcept
{
    Py_CLEAR(self->A);
    Py_CLEAR(self->B);
    Py_CLEAR(self->C);
    Py_CLEAR(self->obj);
    return 0;
}

static void
PrivateSuper_dealloc(PrivateSuperObject* self) noexcept
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->A);
    Py_XDECREF(self->B);
    Py_XDECREF(self->C);
    Py_XDECREF(self->obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject*
PrivateSuper_New(PyTypeObject* type, PyObject* args, PyObject* kwds) noexcept
{
    bool is_class = (type == &PrivateClassSuperType);
    const char* func_name = is_class ? "private_attribute.PrivateClassSuper"
                                     : "private_attribute.PrivateSuper";
    std::string parse = std::string("OOOO:") + func_name;

    PyObject *A, *B, *C, *obj;
    if (!PyArg_ParseTuple(args, parse.c_str(), &A, &B, &C, &obj)) {
        return NULL;
    }
    if (kwds && PyDict_Check(kwds) && PyDict_Size(kwds) > 0) {
        PyErr_SetString(PyExc_TypeError, "PrivateSuper/PrivateClassSuper takes no keyword arguments");
        return NULL;
    }

    PrivateSuperObject* self = (PrivateSuperObject*)type->tp_alloc(type, 0);
    if (!self) {
        return NULL;
    }
    self->A = A;
    self->B = B;
    self->C = C;
    self->obj = obj;
    Py_INCREF(A);
    Py_INCREF(B);
    Py_INCREF(C);
    Py_INCREF(obj);

    if (!private_super_validate(self, is_class)) {
        Py_DECREF(self);
        return NULL;
    }
    return (PyObject*)self;
}

// ===============================================================
// v3: special-method slot structs
// ===============================================================
// These descriptors are installed as __getattribute__ / __getattr__ /
// __setattr__ / __delattr__ / __del__ (and __call__ on metaclasses). CPython's
// update_one_slot rewires the host type's tp_getattro / tp_setattro / tp_call /
// tp_finalize to the standard slot dispatchers, so we no longer set those slots
// directly. Only tp_traverse / tp_clear are set directly (kept from v2).
//
// Two type objects share this C struct:
//   - PrivateAttrSlotType      : for private-attribute *types*. The `name` and
//                                `type` fields are populated via __set_name__.
//   - PrivateAttrMetaSlotType  : for *registered metaclasses*. The `name`,
//                                `type` and `callback` fields are passed in
//                                explicitly during register_metaclass.
typedef struct PrivateAttrSlot {
    PyObject_HEAD
    PyObject* slot_name;    // special-method name ("__getattribute__", ...)
    PyObject* owner_type;   // the type/metaclass this struct is installed on
    PyObject* callback;     // user's original implementation, or NULL
    int is_metaclass;       // 1 for metaclass slots, 0 for type slots
} PrivateAttrSlot;

static void PrivateAttrType_finalize(PyObject* cls) noexcept;
static void clear_object_related(uintptr_t id_self, uintptr_t typ_id) noexcept;
static void PrivateAttr_object_init_private_dict(uintptr_t obj_id, uintptr_t type_id) noexcept;
static bool try_install_pending_creation(PyObject* new_type) noexcept;

static PyMemberDef PrivateAttrSlot_members[] = {
    {"__wrapped__", T_OBJECT_EX, offsetof(PrivateAttrSlot, callback), 0, NULL},
    {NULL}
};

// all: __doc__, __module__, __name__, __qualname__, __annotate__, __type_params__, __isabstractmethod__
static PyObject*
PrivateAttrSlot_get_doc(PrivateAttrSlot* self, void * /*closure*/) noexcept
{
    if (self->callback) {
        PyObject* result = PyObject_GetAttrString((PyObject*)self->callback, "__doc__");
        if (result) {return result;}
        else PyErr_Clear();
    }
    return PyUnicode_FromString("");
}

static PyObject*
PrivateAttrSlot_get_name(PrivateAttrSlot* self, void * /*closure*/) noexcept
{
    if (self->callback) {
        PyObject* result = PyObject_GetAttrString((PyObject*)self->callback, "__name__");
        if (result) {return result;}
        else PyErr_Clear();
    }
    if (self->slot_name) {
        Py_INCREF(self->slot_name);
        return self->slot_name;
    }
    return PyUnicode_FromString("");
}

static PyObject*
PrivateAttrSlot_get_qualname(PrivateAttrSlot* self, void * /*closure*/) noexcept
{
    if (self->callback) {
        PyObject* result = PyObject_GetAttrString((PyObject*)self->callback, "__qualname__");
        if (result) {return result;}
        else PyErr_Clear();
    }
    if (self->slot_name) {
        const char* func_name = PyUnicode_AsUTF8(self->slot_name);
        const char* type_name = "<unknown>";
        if (self->owner_type && PyType_Check(self->owner_type)) {
            type_name = get_name_from_tp_name((PyTypeObject*)self->owner_type);
        }
        return PyUnicode_FromFormat("%s.%s", type_name, func_name);
    }
    return PyUnicode_FromString("");
}

static PyObject*
PrivateAttrSlot_get_module(PrivateAttrSlot* self, void * /*closure*/) noexcept
{
    if (self->callback) {
        PyObject* result = PyObject_GetAttrString((PyObject*)self->callback, "__module__");
        if (result) {return result;}
        else PyErr_Clear();
    }
    return PyUnicode_FromString("private_attribute");
}

static PyObject*
PrivateAttrSlot_get_annotate(PrivateAttrSlot* self, void * /*closure*/) noexcept
{
    if (self->callback) {
        PyObject* result = PyObject_GetAttrString((PyObject*)self->callback, "__annotate__");
        if (result) {return result;}
        else PyErr_Clear();
    }
    Py_RETURN_NONE;
}

static PyObject*
PrivateAttrSlot_get_type_params(PrivateAttrSlot* self, void * /*closure*/) noexcept
{
    if (self->callback) {
        PyObject* result = PyObject_GetAttrString((PyObject*)self->callback, "__type_params__");
        if (result) {return result;}
        else PyErr_Clear();
    }
    Py_RETURN_NONE;
}

static PyObject*
PrivateAttrSlot_get_isabstractmethod(PrivateAttrSlot* self, void * /*closure*/) noexcept
{
    if (self->callback) {
        PyObject* result = PyObject_GetAttrString((PyObject*)self->callback, "__isabstractmethod__");
        if (result) {return result;}
        else PyErr_Clear();
    }
    Py_RETURN_FALSE;
}

static int
PrivateAttrSlot_set_isabstractmethod(PrivateAttrSlot* self, PyObject* value, void * /*closure*/) noexcept
{
    if (self->callback) {
        return PyObject_SetAttrString((PyObject*)self->callback, "__isabstractmethod__", value);
    }
    return 0;
}

static PyGetSetDef PrivateAttrSlot_getsetters[] = {
    {"__doc__", (getter)PrivateAttrSlot_get_doc, NULL, NULL, NULL},
    {"__name__", (getter)PrivateAttrSlot_get_name, NULL, NULL, NULL},
    {"__qualname__", (getter)PrivateAttrSlot_get_qualname, NULL, NULL, NULL},
    {"__module__", (getter)PrivateAttrSlot_get_module, NULL, NULL, NULL},
    {"__annotate__", (getter)PrivateAttrSlot_get_annotate, NULL, NULL, NULL},
    {"__type_params__", (getter)PrivateAttrSlot_get_type_params, NULL, NULL, NULL},
    {"__isabstractmethod__", (getter)PrivateAttrSlot_get_isabstractmethod, (setter)PrivateAttrSlot_set_isabstractmethod, NULL, NULL},
    {NULL}
};

static PyObject*
PrivateAttrSlot_getattro(PyObject* obj, PyObject* name) noexcept
{
    PrivateAttrSlot* self = (PrivateAttrSlot*)obj;
    PyObject* first_result = PyObject_GenericGetAttr(obj, name);
    if (first_result) return first_result;
    else if (self->callback) {
        PyErr_Clear();
        return PyObject_GetAttr((PyObject*)self->callback, name);
    }
    return NULL;
}

// --- public non-private-name cache ---
static bool
is_confirmed_public(uintptr_t obj_id, const std::string& name) noexcept
{
    std::shared_lock<std::shared_mutex> lock(::AllData::public_cache_mutex);
    auto it = ::AllData::public_attr_cache.find(obj_id);
    if (it == ::AllData::public_attr_cache.end()) return false;
    return it->second.find(name) != it->second.end();
}

static void
mark_confirmed_public(uintptr_t obj_id, const std::string& name) noexcept
{
    std::unique_lock<std::shared_mutex> lock(::AllData::public_cache_mutex);
    ::AllData::public_attr_cache[obj_id].insert(name);
}

static void
clear_public_obj(uintptr_t obj_id) noexcept
{
    std::unique_lock<std::shared_mutex> lock(::AllData::public_cache_mutex);
    ::AllData::public_attr_cache.erase(obj_id);
}

static int
slot_traverse(PrivateAttrSlot* self, visitproc visit, void* arg) noexcept
{
    Py_VISIT(self->slot_name);
    Py_VISIT(self->owner_type);
    Py_VISIT(self->callback);
    return 0;
}

static int
slot_clear(PrivateAttrSlot* self) noexcept
{
    Py_CLEAR(self->slot_name);
    Py_CLEAR(self->owner_type);
    Py_CLEAR(self->callback);
    return 0;
}

static void
slot_dealloc(PrivateAttrSlot* self) noexcept
{
    PyObject_GC_UnTrack(self);
    Py_CLEAR(self->slot_name);
    Py_CLEAR(self->owner_type);
    Py_CLEAR(self->callback);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject*
slot_descr_get(PrivateAttrSlot* self, PyObject* obj, PyObject* /*type*/) noexcept
{
    // function/method-like switch: bind `obj` (the accessed object) as the
    // first positional argument, exactly like a Python function descriptor.
    if (obj == NULL || obj == Py_None) {
        Py_INCREF(self);
        return (PyObject*)self;
    }
    return PyMethod_New((PyObject*)self, obj);
}

static PyObject*
slot_set_name(PrivateAttrSlot* self, PyObject* args) noexcept
{
    if (self->owner_type != NULL) {
        Py_RETURN_NONE;
    }
    PyObject *owner, *name;
    if (!PyArg_ParseTuple(args, "OO:__set_name__", &owner, &name)) return NULL;
    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "name must be a string");
        return NULL;
    }
    if (self->slot_name && PyUnicode_Check(self->slot_name)) {
        const char *name_str, *old_name_str;
        name_str = PyUnicode_AsUTF8(name);
        old_name_str = PyUnicode_AsUTF8(self->slot_name);
        if (strcmp(name_str, old_name_str) != 0) {
            PyErr_Format(PyExc_RuntimeError, "the real attribute name '%s' is different from the original name '%s'", name_str, old_name_str);
            return NULL;
        }
    }
    if (!PyType_Check(owner)) {
        PyErr_SetString(PyExc_TypeError, "owner must be a type");
        return NULL;
    }
    if (self->is_metaclass) {
        auto check_need = need_analyse_type(owner);
        if (check_need == AnalyseStatus::Error) return NULL;
        if (check_need == AnalyseStatus::Yes) {
            PyErr_SetString(PyExc_TypeError, "owner is a private type. You may put it in a wrong place");
            return NULL;
        }
        if (!register_metaclass_head(owner)) {
            return NULL;
        }
    } else {
        auto check_need = need_analyse_type(owner);
        if (check_need == AnalyseStatus::Error) return NULL;
        if (check_need == AnalyseStatus::Yes) {
            if (!try_install_pending_creation(owner)) return NULL;
        } else {
            PyErr_SetString(PyExc_TypeError, "owner is not a private type. You may put it in a wrong place");
            return NULL;
        }
    }
    Py_XSETREF(self->owner_type, Py_NewRef(owner));
    Py_XSETREF(self->slot_name, Py_NewRef(name));
    Py_RETURN_NONE;
}

// --- dispatch helpers ---
static PyObject* slot_getattribute_impl(PrivateAttrSlot* self, PyObject* obj, PyObject* name) noexcept;
static PyObject* slot_getattr_impl(PrivateAttrSlot* self, PyObject* obj, PyObject* name) noexcept;
static PyObject* slot_setattr_impl(PrivateAttrSlot* self, PyObject* obj, PyObject* name, PyObject* value) noexcept;
static PyObject* slot_delattr_impl(PrivateAttrSlot* self, PyObject* obj, PyObject* name) noexcept;
static PyObject* slot_del_impl(PrivateAttrSlot* self, PyObject* obj) noexcept;
static PyObject* slot_call_impl(PrivateAttrSlot* self, PyObject* args, PyObject* kwds) noexcept;

// Resolve the type id for an access target. For a metaclass slot the target
// (obj) IS the class object; for a type slot the target is an instance and its
// class is ob_type. The is_metaclass field is the authoritative signal — never
// PyType_Check — so the dispatch is consistent regardless of what obj actually
// is.
static uintptr_t
slot_obj_type_id(PyObject* obj, int is_metaclass) noexcept
{
    return is_metaclass ? (uintptr_t)obj : (uintptr_t)Py_TYPE(obj);
}

// Resolve the type id + object id for an access target, and classify the name.
// Returns the classification result (owner type_id, allowed flag).
static AttrClassifyResult
slot_classify(int is_metaclass, PyObject* obj, const std::string& name_str) noexcept
{
    uintptr_t type_id = slot_obj_type_id(obj, is_metaclass);
    PyObjectStorage code = PyObjectStorage::from_new_ref(get_now_code());
    if (!code) {
        return {true, 0, false};
    }
    return attr_classify(type_id, name_str, (PyCodeObject*)code.get());
}

static TypePrivateAttrResult
slot_is_private(int is_metaclass, PyObject* obj, const std::string& name_str) noexcept
{
    uintptr_t type_id = slot_obj_type_id(obj, is_metaclass);
    return type_private_attr(type_id, name_str);
}

// Emulate `super(owner, obj).<slot_name>(*args, **kwds)`: the parent class /
// metaclass implementation of a special method, resolved from the MRO *after*
// `owner`. This keeps the non-private fallback faithful to Python `super()`
// semantics (respecting parent overrides) instead of bypassing them with
// PyObject_GenericGet/SetAttr.
static PyObject*
super_slot_call(PyObject* owner, PyObject* obj, const char* slot_name, PyObject* args, PyObject* kwds) noexcept
{
    if (!owner) {
        PyErr_SetString(PyExc_RuntimeError, "slot has no owner type");
        return NULL;
    }
    PyObjectStorage super_obj = PyObjectStorage::from_new_ref(
        PyObject_CallFunction((PyObject*)&PySuper_Type, "OO", owner, obj));
    if (!super_obj) return NULL;
    PyObjectStorage method = PyObjectStorage::from_new_ref(
        PyObject_GetAttrString(super_obj, slot_name));
    if (!method) return NULL;
    return PyObject_Call(method, args, kwds);
}

static PyObject*
slot_new_impl(PrivateAttrSlot* self, PyObject* cls, PyObject* args, PyObject* kwds) noexcept
{
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "__new__ must be called with a type");
        return NULL;
    }
    PrivateAttrCreationData data;
    PyTypeObject* typ = (PyTypeObject*)cls;
    const char* type_name = get_name_from_tp_name(typ);
    if (!type_name) {return NULL;}
    if (!PrivateAttrType_preprocess(args, kwds, data, type_name)) return NULL;
    PyObjectStorage type_args = PyObjectStorage::from_new_ref(PyTuple_Pack(4, cls, data.name.get(), data.bases.get(), data.attrs_copy.get()));
    if (!type_args) {
        return nullptr;
    }
    PyObject* final_result = NULL;
    if (self->callback) {
        final_result = PyObject_Call(self->callback, type_args, data.base_kwds);
    } else {
        final_result = super_slot_call(self->owner_type, cls, "__new__", type_args, data.base_kwds);
    }
    if (!final_result) return NULL;
    if (!PyType_Check(final_result)) {
        Py_DECREF(final_result);
        PyErr_SetString(PyExc_TypeError, "metaclass.__new__ must return a type");
        return NULL;
    }
    if (!data.install(final_result)) {
        Py_DECREF(final_result);
        return NULL;
    }
    return final_result;
}

static PyObject*
slot_getattribute_impl(PrivateAttrSlot* self, PyObject* obj, PyObject* name) noexcept
{
    const char* c = PyUnicode_AsUTF8(name);
    if (!c) return NULL;
    std::string name_str(c);
    uintptr_t obj_id = (uintptr_t)obj;
    uintptr_t type_id = slot_obj_type_id(obj, self->is_metaclass);

    if (self->is_metaclass) {
        if (!is_private_meta_type((PyObject*)Py_TYPE(obj))) {
            PyErr_SetString(PyExc_TypeError,
                "The metaclass is not a register type or a subclass of the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
        if (!try_install_pending_creation(obj)) return NULL;
    } else {
        auto check_need_analyse = need_analyse_type((PyObject*)Py_TYPE(obj));
        if (check_need_analyse == AnalyseStatus::Error) {return NULL;}
        if (check_need_analyse == AnalyseStatus::No) {
            PyErr_SetString(PyExc_TypeError,
                "The class is not an instance of register type or the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
        PrivateAttr_object_init_private_dict(obj_id, type_id);
    }

    if (!is_confirmed_public(obj_id, name_str)) {
        TypePrivateAttrResult is_private = slot_is_private(self->is_metaclass, obj, name_str);
        if (!is_private.ok) {
            return NULL;
        }
        if (is_private.is_private) {
            AttrClassifyResult classify = slot_classify(self->is_metaclass, obj, name_str);
            if (!classify.ok) {
                return NULL;
            }
            if (!classify.allowed) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return NULL;
            }
            if (classify.type_id != 0) {
                if (self->is_metaclass) {
                    return type_getattr(classify.type_id, obj, name_str);
                }
                return id_getattr(classify.type_id, name_str, obj, (PyObject*)Py_TYPE(obj));
            }
        }
        mark_confirmed_public(obj_id, name_str);
    }

    // not private (or confirmed public): callback, else super
    if (self->callback) {
        return PyObject_CallFunction(self->callback, "OO", obj, name);
    }
    PyObjectStorage args = PyObjectStorage::from_new_ref(PyTuple_Pack(1, name));
    if (!args) return NULL;
    return super_slot_call(self->owner_type, obj, "__getattribute__", args, NULL);
}

static PyObject*
slot_getattr_impl(PrivateAttrSlot* self, PyObject* obj, PyObject* name) noexcept
{
    const char* c = PyUnicode_AsUTF8(name);
    if (!c) return NULL;
    std::string name_str(c);

    if (self->is_metaclass) {
        if (!is_private_meta_type((PyObject*)Py_TYPE(obj))) {
            PyErr_SetString(PyExc_TypeError,
                "The metaclass is not a register type or a subclass of the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
    } else {
        auto check_need_analyse = need_analyse_type((PyObject*)Py_TYPE(obj));
        if (check_need_analyse == AnalyseStatus::Error) {return NULL;}
        if (check_need_analyse == AnalyseStatus::No) {
            PyErr_SetString(PyExc_TypeError,
                "The class is not an instance of register type or the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
    }

    // Decide the error message: if the name is private, __getattr__ must NOT
    // walk the subsequent flow (callback / super) — go straight to an error.
    {
        TypePrivateAttrResult is_private = slot_is_private(self->is_metaclass, obj, name_str);
        if (!is_private.ok) {
            return NULL;
        }
        if (is_private.is_private) {
            AttrClassifyResult classify = slot_classify(self->is_metaclass, obj, name_str);
            if (!classify.ok) {
                return NULL;
            }
            if (!classify.allowed) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return NULL;
            }
            // genuinely allowed but missing: standard missing-attribute error
            PyTypeObject* typ;
            bool use_self_name = false;
            if (PyType_Check(obj)) {
                typ = (PyTypeObject*)obj;
                use_self_name = true;
            } else {
                typ = Py_TYPE(obj);
            }
            const char* type_name = get_name_from_tp_name(typ);
            if (type_name == NULL) return NULL;
            if (use_self_name) {
                PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%s'", type_name, name_str.c_str());
            } else {
                PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", type_name, name_str.c_str());
            }
            return NULL;
        }
    }

    if (self->callback) {
        return PyObject_CallFunction(self->callback, "OO", obj, name);
    }
    // super().__getattr__(name) if a parent actually defines __getattr__;
    // otherwise fall back to the standard missing-attribute error.
    {
        PyObjectStorage super_obj = PyObjectStorage::from_new_ref(
            PyObject_CallFunction((PyObject*)&PySuper_Type, "OO", self->owner_type, obj));
        if (!super_obj) return NULL;
        int has = PyObject_HasAttrString(super_obj, "__getattr__");
        if (has < 0) return NULL;
        if (has) {
            PyObjectStorage method = PyObjectStorage::from_new_ref(
                PyObject_GetAttrString(super_obj, "__getattr__"));
            if (!method) return NULL;
            PyObjectStorage args = PyObjectStorage::from_new_ref(PyTuple_Pack(1, name));
            if (!args) return NULL;
            return PyObject_Call(method, args, NULL);
        }
    }
    PyTypeObject* typ = PyType_Check(obj) ? (PyTypeObject*)obj : Py_TYPE(obj);
    const char* type_name = get_name_from_tp_name(typ);
    if (type_name == NULL) return NULL;
    if (PyType_Check(obj)) {
        PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%s'", type_name, name_str.c_str());
    } else {
        PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", type_name, name_str.c_str());
    }
    return NULL;
}

static PyObject*
slot_setattr_impl(PrivateAttrSlot* self, PyObject* obj, PyObject* name, PyObject* value) noexcept
{
    const char* c = PyUnicode_AsUTF8(name);
    if (!c) return NULL;
    std::string name_str(c);
    uintptr_t obj_id = (uintptr_t)obj;
    uintptr_t type_id = slot_obj_type_id(obj, self->is_metaclass);

    if (self->is_metaclass) {
        if (!is_private_meta_type((PyObject*)Py_TYPE(obj))) {
            PyErr_SetString(PyExc_TypeError,
                "The metaclass is not a register type or a subclass of the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
        if (!try_install_pending_creation(obj)) return NULL;
    } else {
        auto check_need_analyse = need_analyse_type((PyObject*)Py_TYPE(obj));
        if (check_need_analyse == AnalyseStatus::Error) {return NULL;}
        if (check_need_analyse == AnalyseStatus::No) {
            PyErr_SetString(PyExc_TypeError,
                "The class is not an instance of register type or the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
        PrivateAttr_object_init_private_dict(obj_id, type_id);
    }

    if (!is_confirmed_public(obj_id, name_str)) {
        TypePrivateAttrResult is_private = slot_is_private(self->is_metaclass, obj, name_str);
        if (!is_private.ok) {
            return NULL;
        }
        if (is_private.is_private) {
            AttrClassifyResult classify = slot_classify(self->is_metaclass, obj, name_str);
            if (!classify.ok) {
                return NULL;
            }
            if (!classify.allowed) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return NULL;
            }
            if (classify.type_id != 0) {
                int r;
                if (self->is_metaclass) {
                    r = type_setattr(classify.type_id, obj, name_str, value);
                } else {
                    r = id_setattr(classify.type_id, name_str, obj, (PyObject*)Py_TYPE(obj), value);
                }
                if (r < 0) return NULL;
                Py_RETURN_NONE;
            }
        }
        mark_confirmed_public(obj_id, name_str);
    }

    if (self->callback) {
        return PyObject_CallFunction(self->callback, "OOO", obj, name, value);
    }
    PyObjectStorage args = PyObjectStorage::from_new_ref(PyTuple_Pack(2, name, value));
    if (!args) return NULL;
    PyObjectStorage r = PyObjectStorage::from_new_ref(super_slot_call(self->owner_type, obj, "__setattr__", args, NULL));
    if (!r) return NULL;
    Py_RETURN_NONE;
}

static PyObject*
slot_delattr_impl(PrivateAttrSlot* self, PyObject* obj, PyObject* name) noexcept
{
    const char* c = PyUnicode_AsUTF8(name);
    if (!c) return NULL;
    std::string name_str(c);
    uintptr_t obj_id = (uintptr_t)obj;
    uintptr_t type_id = slot_obj_type_id(obj, self->is_metaclass);

    if (self->is_metaclass) {
        if (!is_private_meta_type((PyObject*)Py_TYPE(obj))) {
            PyErr_SetString(PyExc_TypeError,
                "The metaclass is not a register type or a subclass of the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
        if (!try_install_pending_creation(obj)) return NULL;
    } else {
        auto check_need_analyse = need_analyse_type((PyObject*)Py_TYPE(obj));
        if (check_need_analyse == AnalyseStatus::Error) {return NULL;}
        if (check_need_analyse == AnalyseStatus::No) {
            PyErr_SetString(PyExc_TypeError,
                "The class is not an instance of register type or the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
        PrivateAttr_object_init_private_dict(obj_id, type_id);
    }

    if (!is_confirmed_public(obj_id, name_str)) {
        TypePrivateAttrResult is_private = slot_is_private(self->is_metaclass, obj, name_str);
        if (!is_private.ok) {
            return NULL;
        }
        if (is_private.is_private) {
            AttrClassifyResult classify = slot_classify(self->is_metaclass, obj, name_str);
            if (!classify.ok) {
                return NULL;
            }
            if (!classify.allowed) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return NULL;
            }
            if (classify.type_id != 0) {
                int r;
                if (self->is_metaclass) {
                    r = type_delattr(classify.type_id, obj, name_str);
                } else {
                    r = id_delattr(classify.type_id, name_str, obj, (PyObject*)Py_TYPE(obj));
                }
                if (r < 0) return NULL;
                Py_RETURN_NONE;
            }
        }
        mark_confirmed_public(obj_id, name_str);
    }

    if (self->callback) {
        PyObjectStorage none = PyObjectStorage::from_new_ref(PyObject_CallFunction(self->callback, "OO", obj, name));
        if (!none) return NULL;
        Py_RETURN_NONE;
    }
    PyObjectStorage args = PyObjectStorage::from_new_ref(PyTuple_Pack(1, name));
    if (!args) return NULL;
    PyObjectStorage r = PyObjectStorage::from_new_ref(super_slot_call(self->owner_type, obj, "__delattr__", args, NULL));
    if (!r) return NULL;
    Py_RETURN_NONE;
}

static PyObject*
slot_del_impl(PrivateAttrSlot* self, PyObject* obj) noexcept
{
    if (self->is_metaclass) {
        if (!is_private_meta_type((PyObject*)Py_TYPE(obj))) {
            PyErr_SetString(PyExc_TypeError,
                "The metaclass is not a register type or a subclass of the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
    } else {
        auto check_need_analyse = need_analyse_type((PyObject*)Py_TYPE(obj));
        if (check_need_analyse == AnalyseStatus::Error) {return NULL;}
        if (check_need_analyse == AnalyseStatus::No) {
            PyErr_SetString(PyExc_TypeError,
                "The class is not an instance of register type or the PrivateAttrType! You may put it on a wrong place");
            return NULL;
        }
    }
    bool should_return_null = false;
    auto original_refcnt = Py_REFCNT(obj);
    if (self->callback) {
        PyObjectStorage r = PyObjectStorage::from_new_ref(PyObject_CallFunction(self->callback, "O", obj));
        if (!r) should_return_null = true;
    } else {
        // Chain to a parent's __del__ ONLY if one actually exists. If no
        // parent defines __del__, do nothing (no callback invocation, no
        // error). A parent's __del__ may resurrect the object (bump refcount);
        // CPython handles that by re-scheduling finalization.
        PyObjectStorage super_obj = PyObjectStorage::from_new_ref(
            PyObject_CallFunction((PyObject*)&PySuper_Type, "OO", self->owner_type, obj));
        if (super_obj) {
            int has = PyObject_HasAttrString(super_obj, "__del__");
            if (has > 0) {
                PyObjectStorage method = PyObjectStorage::from_new_ref(
                    PyObject_GetAttrString(super_obj, "__del__"));
                if (method) {
                    PyObjectStorage empty = PyObjectStorage::from_new_ref(PyTuple_New(0));
                    if (empty) {
                        PyObjectStorage r = PyObjectStorage::from_new_ref(PyObject_Call(method, empty, NULL));
                        if (!r) should_return_null = true;
                    }
                }
            }
        }
    }
    if (Py_REFCNT(obj) > original_refcnt) goto final_return;
    if (self->is_metaclass) {
        PrivateAttrType_finalize(obj);
    } else {
        clear_object_related((uintptr_t)obj, (uintptr_t)Py_TYPE(obj));
    }
final_return:
    if (should_return_null) return NULL;
    Py_RETURN_NONE;
}

static PyObject*
slot_call_impl(PrivateAttrSlot* self, PyObject* args, PyObject* kwds) noexcept
{
    // metaclass __call__: args = (cls, *call_args)
    if (!self->is_metaclass) {
        PyErr_SetString(PyExc_TypeError, "__call__ can only be applyed on register metaclass");
        return NULL;
    }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n == 0) {
        PyErr_SetString(PyExc_TypeError, "metaclass __call__ requires the type itself");
        return NULL;
    }
    PyObject* cls = PyTuple_GET_ITEM(args, 0);
    if (!try_install_pending_creation(cls)) return NULL;
    if (self->callback) {
        return PyObject_Call(self->callback, args, kwds);
    }
    PyObjectStorage rest = PyObjectStorage::from_new_ref(PyTuple_GetSlice(args, 1, n));
    if (!rest) return NULL;
    return super_slot_call(self->owner_type, cls, "__call__", rest, kwds);
}

static PyObject*
slot_call(PrivateAttrSlot* self, PyObject* args, PyObject* kwds) noexcept
{
    const char* sn = PyUnicode_AsUTF8(self->slot_name);
    if (!sn) return NULL;
    const char* type_name = get_name_from_tp_name((PyTypeObject*)(self->owner_type));
    if (strcmp(sn, "__getattribute__") == 0) {
        PyObject *obj, *name;
        std::string format = "OO:" + std::string(type_name) + "." + std::string(sn);
        if (!PyArg_ParseTuple(args, format.c_str(), &obj, &name)) return NULL;
        return slot_getattribute_impl(self, obj, name);
    }
    if (strcmp(sn, "__getattr__") == 0) {
        PyObject *obj, *name;
        std::string format = "OO:" + std::string(type_name) + "." + std::string(sn);
        if (!PyArg_ParseTuple(args, format.c_str(), &obj, &name)) return NULL;
        return slot_getattr_impl(self, obj, name);
    }
    if (strcmp(sn, "__setattr__") == 0) {
        PyObject *obj, *name, *value;
        std::string format = "OOO:" + std::string(type_name) + "." + std::string(sn);
        if (!PyArg_ParseTuple(args, format.c_str(), &obj, &name, &value)) return NULL;
        return slot_setattr_impl(self, obj, name, value);
    }
    if (strcmp(sn, "__delattr__") == 0) {
        PyObject *obj, *name;
        std::string format = "OO:" + std::string(type_name) + "." + std::string(sn);
        if (!PyArg_ParseTuple(args, format.c_str(), &obj, &name)) return NULL;
        return slot_delattr_impl(self, obj, name);
    }
    if (strcmp(sn, "__del__") == 0) {
        PyObject *obj;
        std::string format = "O:" + std::string(type_name) + "." + std::string(sn);
        if (!PyArg_ParseTuple(args, format.c_str(), &obj)) return NULL;
        return slot_del_impl(self, obj);
    }
    if (strcmp(sn, "__call__") == 0) {
        return slot_call_impl(self, args, kwds);
    }
    PyErr_Format(PyExc_TypeError, "unknown special-method slot '%s'", sn);
    return NULL;
}

static PyObject*
slot_new_call(PrivateAttrSlot* self, PyObject* args, PyObject* kwds) noexcept
{
    // check the length of the args is 4
    if (PyTuple_GET_SIZE(args) != 4) {
        PyErr_SetString(PyExc_TypeError, "slot_new_call requires 4 arguments");
        return NULL;
    }
    // split first and the other
    PyObject* first = PyTuple_GET_ITEM(args, 0);
    // use split to get the other
    PyObject* other = PyTuple_GetSlice(args, 1, 4);
    if (!other) return NULL;
    // call the slot with the other
    return slot_new_impl(self, first, other, kwds);
}

static PyMethodDef PrivateAttrSlot_methods[] = {
    {"__set_name__", (PyCFunction)slot_set_name, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject PrivateAttrSlotType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute._PrivateAttrSlot",      // tp_name
    sizeof(PrivateAttrSlot),                   // tp_basicsize
    0,                                         // tp_itemsize
    (destructor)slot_dealloc,                  // tp_dealloc
    0,                                         // tp_print
    0,                                         // tp_getattr
    0,                                         // tp_setattr
    0,                                         // tp_reserved
    0,                                         // tp_repr
    0,                                         // tp_as_number
    0,                                         // tp_as_sequence
    0,                                         // tp_as_mapping
    0,                                         // tp_hash
    (ternaryfunc)slot_call,                    // tp_call
    0,                                         // tp_str
    PrivateAttrSlot_getattro,                  // tp_getattro
    0,                                         // tp_setattro
    0,                                         // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,   // tp_flags
    "private attribute access slot",           // tp_doc
    (traverseproc)slot_traverse,               // tp_traverse
    (inquiry)slot_clear,                       // tp_clear
    0,                                         // tp_richcompare
    0,                                         // tp_weaklistoffset
    0,                                         // tp_iter
    0,                                         // tp_iternext
    PrivateAttrSlot_methods,                   // tp_methods
    PrivateAttrSlot_members,                   // tp_members
    PrivateAttrSlot_getsetters,                // tp_getset
    0,                                         // tp_base
    0,                                         // tp_dict
    (descrgetfunc)slot_descr_get,              // tp_descr_get
    0,                                         // tp_descr_set
};

static PyTypeObject PrivateAttrMetaSlotType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute._PrivateAttrMetaSlot",  // tp_name
    sizeof(PrivateAttrSlot),                   // tp_basicsize
    0,                                         // tp_itemsize
    (destructor)slot_dealloc,                  // tp_dealloc
    0,                                         // tp_print
    0,                                         // tp_getattr
    0,                                         // tp_setattr
    0,                                         // tp_reserved
    0,                                         // tp_repr
    0,                                         // tp_as_number
    0,                                         // tp_as_sequence
    0,                                         // tp_as_mapping
    0,                                         // tp_hash
    (ternaryfunc)slot_call,                    // tp_call
    0,                                         // tp_str
    PrivateAttrSlot_getattro,                  // tp_getattro
    0,                                         // tp_setattro
    0,                                         // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,   // tp_flags
    "metaclass private attribute access slot", // tp_doc
    (traverseproc)slot_traverse,               // tp_traverse
    (inquiry)slot_clear,                       // tp_clear
    0,                                         // tp_richcompare
    0,                                         // tp_weaklistoffset
    0,                                         // tp_iter
    0,                                         // tp_iternext
    PrivateAttrSlot_methods,                   // tp_methods
    PrivateAttrSlot_members,                   // tp_members
    PrivateAttrSlot_getsetters,                // tp_getset
    0,                                         // tp_base
    0,                                         // tp_dict
    (descrgetfunc)slot_descr_get,              // tp_descr_get
    0,                                         // tp_descr_set
};

static PyTypeObject PrivateAttrMetaNewSlotType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute._PrivateAttrMetaNewSlot",  // tp_name
    sizeof(PrivateAttrSlot),                   // tp_basicsize
    0,                                         // tp_itemsize
    (destructor)slot_dealloc,                  // tp_dealloc
    0,                                         // tp_print
    0,                                         // tp_getattr
    0,                                         // tp_setattr
    0,                                         // tp_reserved
    0,                                         // tp_repr
    0,                                         // tp_as_number
    0,                                         // tp_as_sequence
    0,                                         // tp_as_mapping
    0,                                         // tp_hash
    (ternaryfunc)slot_new_call,                // tp_call
    0,                                         // tp_str
    PrivateAttrSlot_getattro,                  // tp_getattro
    0,                                         // tp_setattro
    0,                                         // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,   // tp_flags
    "metaclass private attribute new slot",    // tp_doc
    (traverseproc)slot_traverse,               // tp_traverse
    (inquiry)slot_clear,                       // tp_clear
    0,                                         // tp_richcompare
    0,                                         // tp_weaklistoffset
    0,                                         // tp_iter
    0,                                         // tp_iternext
    PrivateAttrSlot_methods,                   // tp_methods
    PrivateAttrSlot_members,                   // tp_members
    PrivateAttrSlot_getsetters,                // tp_getset
};

static const char* type_slot_names[] = {
    "__getattribute__", "__getattr__", "__setattr__", "__delattr__", "__del__", NULL
};

static const char* metaclass_slot_names[] = {
    "__getattribute__", "__getattr__", "__setattr__", "__delattr__", "__del__", "__call__", NULL
};

static PrivateAttrSlot*
slot_new_common(const char* name, PyObject* owner, PyObject* callback, int is_metaclass) noexcept
{
    PyTypeObject* tp = is_metaclass ? &PrivateAttrMetaSlotType : &PrivateAttrSlotType;
    PrivateAttrSlot* s = PyObject_GC_New(PrivateAttrSlot, tp);
    if (!s) return NULL;
    s->slot_name = PyUnicode_InternFromString(name);
    s->owner_type = owner ? (PyObject*)Py_NewRef(owner) : NULL;
    s->callback = callback ? (PyObject*)Py_NewRef(callback) : NULL;
    s->is_metaclass = is_metaclass;
    if (!s->slot_name) {
        Py_DECREF(s);
        return NULL;
    }
    PyObject_GC_Track(s);
    return s;
}

static PrivateAttrSlot*
create_type_new_slot(PyObject* owner, PyObject* callback) noexcept
{
    PrivateAttrSlot* result = PyObject_GC_New(PrivateAttrSlot, &PrivateAttrMetaNewSlotType);
    if (!result) return NULL;
    result->slot_name = PyUnicode_InternFromString("__new__");
    if (!result->slot_name) {
        Py_DECREF(result);
        return NULL;
    }
    result->owner_type = owner ? (PyObject*)Py_NewRef(owner) : NULL;
    result->callback = callback ? (PyObject*)Py_NewRef(callback) : NULL;
    result->is_metaclass = 1;
    PyObject_GC_Track(result);
    return result;
}

static PyObject*
register_type_create_new(PyObject* /*self*/, PyObject* args) noexcept
{
    PyObject* callback = NULL;
    if (!PyArg_ParseTuple(args, "|O:private_attribute.create_new", &callback)) {
        return NULL;
    }
    return (PyObject*)create_type_new_slot(NULL, callback);
}

static PyObject*
register_type_create_slot_wrapper(PyObject* name, PyObject* args) noexcept
{
    // is_metaclass is 1
    const char* name_str = PyUnicode_AsUTF8(name);
    PyObject* callback = NULL;
    if (!PyArg_ParseTuple(args, "|O:create_register_type_slot_wrapper", &callback)) {
        return NULL;
    }
    if (!name_str) return NULL;
    return (PyObject*)slot_new_common(name_str, NULL, callback, 1);
}

static PyMethodDef register_type_create_slots_wrapper_def = {
    "create_register_type_slot_wrapper", register_type_create_slot_wrapper, METH_VARARGS, NULL
};

static PyObject*
register_type_create_slot(PyObject* /*self*/, PyObject* name) noexcept
{
    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "name must be a string");
        return NULL;
    }
    const char* name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return NULL;
    bool allowed = false;
    for (int i = 0; metaclass_slot_names[i]; i++) {
        if (strcmp(name_str, metaclass_slot_names[i]) == 0) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        PyErr_Format(PyExc_TypeError, "name '%s' is not allowed", name_str);
        return NULL;
    }
    return PyCFunction_New(&register_type_create_slots_wrapper_def, name);
}

static PyObject*
private_type_creatr_slot_wrapper(PyObject* name, PyObject* args) noexcept
{
    const char* name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return NULL;
    PyObject* callback = NULL;
    if (!PyArg_ParseTuple(args, "|O:create_private_type_slot_wrapper", &callback)) {
        return NULL;
    }
    return (PyObject*)slot_new_common(name_str, NULL, callback, 0);
}

static PyMethodDef private_type_create_slots_wrapper_def = {
    "create_private_type_slot_wrapper", private_type_creatr_slot_wrapper, METH_VARARGS, NULL
};

static PyObject*
private_type_create_slot(PyObject* /*self*/, PyObject* name) noexcept
{
    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "name must be a string");
        return NULL;
    }
    const char* name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return NULL;
    bool allowed = false;
    for (int i = 0; type_slot_names[i]; i++) {
        if (strcmp(name_str, type_slot_names[i]) == 0) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        PyErr_Format(PyExc_TypeError, "name '%s' is not allowed", name_str);
        return NULL;
    }
    return PyCFunction_New(&private_type_create_slots_wrapper_def, name);
}

static PyObject*
make_type_slot(const char* name, PyObject* callback, PyObject* owner) noexcept
{
    return (PyObject*)slot_new_common(name, NULL, callback, 0);
}

static PyObject*
make_metaclass_slot(const char* name, PyObject* owner, PyObject* callback) noexcept
{
    return (PyObject*)slot_new_common(name, owner, callback, 1);
}

// Inject the special-method structs into a class namespace (attrs dict). For
// each special name present in attrs, the user's value becomes the callback;
// the struct replaces it in the dict so type_new wires the slots via
// update_one_slot and invokes __set_name__.
static bool
inject_type_slots(PyObject* attrs) noexcept
{
    for (int i = 0; type_slot_names[i]; i++) {
        const char* nm = type_slot_names[i];
        int has = PyDict_ContainsString(attrs, nm);
        if (has < 0) return false;
        PyObject* callback = NULL;
        if (has) {
            callback = PyDict_GetItemString(attrs, nm);
        }
        while (callback && (
            PyObject_IsInstance(callback, (PyObject*)&PrivateAttrMetaSlotType) ||
            PyObject_IsInstance(callback, (PyObject*)&PrivateAttrMetaNewSlotType) ||
            (PyObject_IsInstance(callback, (PyObject*)&PrivateAttrSlotType) && ((PrivateAttrSlot*)callback)->owner_type != NULL)
        )) {
            callback = ((PrivateAttrSlot*)callback) -> callback;
        }
        if (callback && PyObject_IsInstance(callback, (PyObject*)&PrivateAttrSlotType) && ((PrivateAttrSlot*)callback)->owner_type == NULL) {
            continue;
        }
        PyObject* slot = make_type_slot(nm, callback, NULL);
        if (!slot) return false;
        int r = PyDict_SetItemString(attrs, nm, slot);
        Py_DECREF(slot);
        if (r < 0) return false;
    }
    return true;
}

// Install the special-method structs into a registered metaclass's dict.
// The owner is passed explicitly. If the metaclass already defines one of the
// names, its value becomes the callback. Must use the ORIGINAL type_setattro
// (not PyObject_SetAttr, which would route through new_type_tp_setattro and
// short-circuit via update_slot_callback), so update_one_slot actually rewires
// the metaclass's tp_getattro / tp_setattro / tp_call slots.
static bool
install_metaclass_slots(PyTypeObject* metaclass) noexcept
{
    PyObject* dict = metaclass->tp_dict;
    setattrofunc real_setattro = original_type_tp_setattro ? original_type_tp_setattro : PyType_Type.tp_setattro;
    for (int i = 0; metaclass_slot_names[i]; i++) {
        const char* nm = metaclass_slot_names[i];
        int has = PyDict_ContainsString(dict, nm);
        if (has < 0) return false;
        PyObject* callback = NULL;
        if (has) {
            PyObject* existing = PyDict_GetItemString(dict, nm);
            if (!existing) return false;
            // skip over an already-installed struct of ours
            if (PyObject_IsInstance(existing, (PyObject*)&PrivateAttrMetaSlotType)) {
                continue;
            } else {
                callback = existing;
            }
        }
        PyObjectStorage slot = PyObjectStorage::from_new_ref(make_metaclass_slot(nm, (PyObject*)metaclass, callback));
        if (!slot) return false;
        PyObjectStorage name_obj = PyObjectStorage::from_new_ref(PyUnicode_FromString(nm));
        if (!name_obj) {
            return false;
        }
        int r = real_setattro((PyObject*)metaclass, name_obj, slot);
        if (r < 0) {
            return false;
        }
    }
    const char* new_name = "__new__";
    int has = PyDict_ContainsString(dict, new_name);
    if (has < 0) return false;
    if (has) {
        PyObject* existing = PyDict_GetItemString(dict, new_name);
        if (!existing) {return false;}
        if (!PyObject_IsInstance(existing, (PyObject*)&PrivateAttrMetaNewSlotType)) {
            auto* new_slot = create_type_new_slot((PyObject*)metaclass, NULL);
            PyObjectStorage new_slot_storage = PyObjectStorage::from_new_ref((PyObject*)new_slot);
            if (!new_slot_storage) {
                return false;
            }
            PyObjectStorage name_obj = PyObjectStorage::from_new_ref(PyUnicode_FromString(new_name));
            if (!name_obj) {
                return false;
            }
            int r = real_setattro((PyObject*)metaclass, name_obj, new_slot_storage);
            if (r < 0) {
                return false;
            }
        }
    }
    return true;
}

// Write a new user callback into an installed slot struct's callback field.
//
// If the slot name already exists and is one of our structs, we try to reuse it
// in place: for a PrivateAttrMetaSlotType struct whose owner_type still matches
// `owner`, only the callback field is rewritten (no re-creation). Otherwise we
// fall through and rebuild the struct in place. If the existing value is NOT one
// of our slot structs at all, we also rebuild in place instead of giving up, so
// the original attribute is preserved as the new callback.
static bool
update_slot_callback(PyTypeObject* owner, const char* name, PyObject* callback, int is_metaclass) noexcept
{
    PyObject* dict = owner->tp_dict;
    if (!dict) return false;
    PyObject* slot = PyDict_GetItemString(dict, name);

    // Fast path: reuse an existing struct when possible.
    if (slot && PyObject_IsInstance(slot, (PyObject*)&PrivateAttrMetaSlotType)) {
        PrivateAttrSlot* s = (PrivateAttrSlot*)slot;
        if (is_metaclass) {
            Py_XSETREF(s->callback, callback ? (PyObject*)Py_NewRef(callback) : NULL);
            return true;
        }
    } else if (slot && PyObject_IsInstance(slot, (PyObject*)&PrivateAttrSlotType)) {
        PrivateAttrSlot* s = (PrivateAttrSlot*)slot;
        if (!is_metaclass) {
            Py_XSETREF(s->callback, callback ? (PyObject*)Py_NewRef(callback) : NULL);
            return true;
        }
    }

    // Rebuild the slot struct in place, preserving the original value (if any)
    // as the new callback. This covers:
    //   - no existing slot at all,
    //   - an existing struct with a mismatched owner_type,
    //   - an existing value that is not one of our slot structs.
    PyObjectStorage new_slot = PyObjectStorage::from_new_ref(is_metaclass
        ? make_metaclass_slot(name, (PyObject*)owner, callback)
        : make_type_slot(name, callback, (PyObject*)owner));
    if (!new_slot) return false;

    setattrofunc real_setattro = original_type_tp_setattro ? original_type_tp_setattro : PyType_Type.tp_setattro;
    PyObjectStorage name_obj = PyObjectStorage::from_new_ref(PyUnicode_FromString(name));
    if (!name_obj) {
        return false;
    }
    int r = real_setattro((PyObject*)owner, name_obj, new_slot);
    return r >= 0;
}

static bool
update_metaclass_new(PyTypeObject* owner, PyObject* callback) noexcept
{
    PyObject* dict = owner->tp_dict;
    if (!dict) return false;

    PyObject* now_new = PyDict_GetItemString(dict, "__new__");
    if (now_new && PyObject_IsInstance(now_new, (PyObject*)&PrivateAttrMetaNewSlotType)) {
        PrivateAttrSlot* s = (PrivateAttrSlot*)now_new;
        Py_XSETREF(s->callback, callback ? (PyObject*)Py_NewRef(callback) : NULL);
        return true;
    }
    PrivateAttrSlot* new_slot = create_type_new_slot((PyObject*)owner, callback);
    if (!new_slot) return false;
    PyObjectStorage new_slot_storage = PyObjectStorage::from_new_ref((PyObject*)new_slot);

    setattrofunc real_setattro = original_type_tp_setattro ? original_type_tp_setattro : PyType_Type.tp_setattro;
    PyObjectStorage name_obj = PyObjectStorage::from_new_ref(PyUnicode_FromString("__new__"));
    if (!name_obj) {
        return false;
    }
    int r = real_setattro((PyObject*)owner, name_obj, new_slot_storage);
    return r >= 0;
}

// ===============================================================
// PrivateAttrType
// ===============================================================
typedef struct {
    PyHeapTypeObject base; // PyObject_HEAD_INIT(NULL)
} PrivateAttrTypeObject;

static void PrivateAttr_object_init_private_dict(uintptr_t obj_id, uintptr_t type_id) noexcept;
static void clear_object_related(uintptr_t id_self, uintptr_t typ_id) noexcept;

// shared helper for clearing object-related data (used by tp_clear and tp_finalize)
static void
clear_object_related(uintptr_t id_self, uintptr_t typ_id) noexcept
{
    std::vector<uintptr_t> parent_ids;
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()){
        parent_ids = ::AllData::all_type_parent_id[typ_id];
    }

    // first: clear ::AllData::all_object_attr and ::AllData::all_object_mutex on this typ_id
    if (::AllData::all_object_attr.find(typ_id) != ::AllData::all_object_attr.end()){
        auto& all_object_attr = ::AllData::all_object_attr[typ_id];
        if (all_object_attr.find(id_self) != all_object_attr.end()){
            all_object_attr.erase(id_self);
        }
    }
    if (::AllData::all_object_mutex.find(typ_id) != ::AllData::all_object_mutex.end()){
        auto& all_object_mutex = ::AllData::all_object_mutex[typ_id];
        if (all_object_mutex.find(id_self) != all_object_mutex.end()){
            all_object_mutex.erase(id_self);
        }
    }
    // second: clear the above in parent types
    for (auto& parent_id : parent_ids){
        if (::AllData::all_object_attr.find(parent_id) != ::AllData::all_object_attr.end()){
            auto& all_object_attr = ::AllData::all_object_attr[parent_id];
            if (all_object_attr.find(id_self) != all_object_attr.end()){
                all_object_attr.erase(id_self);
            }
        }
        if (::AllData::all_object_mutex.find(parent_id) != ::AllData::all_object_mutex.end()){
            auto& all_object_mutex = ::AllData::all_object_mutex[parent_id];
            if (all_object_mutex.find(id_self) != all_object_mutex.end()){
                all_object_mutex.erase(id_self);
            }
        }
    }
    clear_obj(id_self);
}

// ========================================================================
// Re-entrancy guard for the traverse/clear wrappers.
//
// The wrappers MUST keep calling the saved "original" slot functions: a
// wrapped metaclass may have been created by another C-level metaclass, so we
// cannot tell whether the saved function is a real custom implementation or a
// delegating default such as CPython's subtype_traverse/subtype_clear. Those
// defaults read the LIVE tp_traverse/tp_clear slot and delegate back into our
// wrapper, which would otherwise recurse infinitely.
//
// The guard makes a re-entrant call for the same object a no-op (returns 0),
// so the outermost invocation finishes the whole traversal/clear exactly once.
// It is thread-local so it is safe for free-threaded builds (Py_GIL_DISABLED).
// ========================================================================
class PrivateAttrRecursionGuard
{
private:
    static std::unordered_set<uintptr_t>& in_flight() noexcept {
        static thread_local std::unordered_set<uintptr_t> set;
        return set;
    }

    uintptr_t key;

public:
    // Returns false when `key` is already being traversed/cleared by an outer
    // invocation of the same wrapper (i.e. this call is a re-entrant one).
    static bool try_enter(uintptr_t key) noexcept {
        auto& set = in_flight();
        if (set.find(key) != set.end()) {
            return false;
        }
        set.insert(key);
        return true;
    }

    explicit PrivateAttrRecursionGuard(uintptr_t key) noexcept : key(key) {}

    ~PrivateAttrRecursionGuard() noexcept {
        in_flight().erase(key);
    }
};

// ========================================================================
// __slots__ traversal/clear (mirror of CPython's subtype_traverse slot loop).
//
// A private-attribute class's LIVE tp_traverse/tp_clear are replaced with
// PrivateAttr_tp_traverse / PrivateAttr_tp_clear. The saved "original" is
// CPython's subtype_traverse/subtype_clear, but because subtype_traverse
// resolves the nearest base through the LIVE slot, it treats our wrapper as
// the nearest "different" traverse and skips its own traverse_slots loop. The
// __slots__ members (Py_T_OBJECT_EX entries in the heap type's member table)
// therefore become invisible to the collector, so a cycle formed purely
// through __slots__ members leaks.
//
// These helpers walk the member table for each type level whose live
// traverse/clear slot is still a delegating default (subtype_traverse /
// subtype_clear or our own wrapper), exactly mirroring the
// `while (base->tp_traverse == subtype_traverse)` walk in CPython.
// ========================================================================

// Locate the member table of a heap type in a version-independent way.
// - 3.12+ : PyObject_GetItemData((PyObject*)type) is the public accessor
//           (_PyHeapType_GET_MEMBERS is defined as that since 3.12).
// - <=3.11 : members float right after the PyHeapTypeObject header:
//           _PyHeapType_GET_MEMBERS(et) == (char*)et + Py_TYPE(et)->tp_basicsize.
static PyMemberDef*
PrivateAttr_get_tp_members(PyTypeObject* type) noexcept
{
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        return NULL;
    }
#if PY_VERSION_HEX >= 0x030C0000
    return (PyMemberDef*)PyObject_GetItemData((PyObject*)type);
#else
    return (PyMemberDef*)((char*)type + Py_TYPE(type)->tp_basicsize);
#endif
}

static void *
PrivateAttr_Member_GetOffset(PyObject *obj, PyMemberDef *mp) noexcept
{
    unsigned char *addr = (unsigned char *)obj + mp->offset;
// only python >= 3.15 has _Py_AFTER_ITEMS
#if PY_VERSION_HEX >= 0x030F0000
    if (mp->flags & _Py_AFTER_ITEMS) {
        PyTypeObject *type = Py_TYPE(obj);
        addr += _Py_SIZE_ROUND_UP(Py_SIZE(obj) * type->tp_itemsize, SIZEOF_VOID_P);
    }
#endif
    return addr;
}

static int
PrivateAttr_traverse_slots(PyTypeObject* type, PyObject* self,
                           visitproc visit, void* arg) noexcept
{
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        return 0;
    }
    PyMemberDef* mp = PrivateAttr_get_tp_members(type);
    if (mp == NULL) {
        return 0;
    }
    Py_ssize_t n = Py_SIZE(type);
    for (Py_ssize_t i = 0; i < n; i++, mp++) {
        if (mp->type == T_OBJECT_EX) {
            void* addr = PrivateAttr_Member_GetOffset(self, mp);
            PyObject* obj = *(PyObject**)addr;
            if (obj != NULL) {
                int err = visit(obj, arg);
                if (err) {
                    return err;
                }
            }
        }
    }
    return 0;
}

static void
PrivateAttr_clear_slots(PyTypeObject* type, PyObject* self) noexcept
{
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        return;
    }
    PyMemberDef* mp = PrivateAttr_get_tp_members(type);
    if (mp == NULL) {
        return;
    }
    Py_ssize_t n = Py_SIZE(type);
    for (Py_ssize_t i = 0; i < n; i++, mp++) {
        if (mp->type == T_OBJECT_EX && !(mp->flags & READONLY)) {
            void* addr = PrivateAttr_Member_GetOffset(self, mp);
            PyObject* obj = *(PyObject**)addr;
            if (obj != NULL) {
                *(PyObject**)addr = NULL;
                Py_DECREF(obj);
            }
        }
    }
}

static int
PrivateAttr_tp_traverse(PyObject* self, visitproc visit, void* arg) noexcept
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t typ_id = (uintptr_t)typ;
    uintptr_t id_self = (uintptr_t)self;

    // Re-entrancy guard: see PrivateAttrRecursionGuard above.
    if (!PrivateAttrRecursionGuard::try_enter(id_self)) {
        return 0;
    }
    PrivateAttrRecursionGuard guard(id_self);

    // call original traverse if exists
    if (::AllData::all_type_traverse.find(typ_id) != ::AllData::all_type_traverse.end()) {
        traverseproc orig = ::AllData::all_type_traverse[typ_id];
        if (orig) {
            int res = orig(self, visit, arg);
            if (res) return res;
        }
    } else {
        traverseproc orig = get_need_tp_traverse(typ);
        if (orig) {
            int res = orig(self, visit, arg);
            if (res) return res;
        }
    }

    // __slots__ visit (mirror of subtype_traverse's slot-walking loop). See
    // PrivateAttr_traverse_slots above: once the LIVE tp_traverse is this
    // wrapper, subtype_traverse skips its own loop, so walk the member table
    // of this type and every delegating base explicitly. Stop at the first
    // base whose live traverse is a real implementation (it owns its slots).
    {
        PyTypeObject* base = typ;
        while (base) {
            if (base->tp_traverse == ::AllData::captured_subtype_traverse
                || base->tp_traverse == PrivateAttr_tp_traverse) {
                if (Py_SIZE(base)) {
                    int err = PrivateAttr_traverse_slots(base, self, visit, arg);
                    if (err) {
                        return err;
                    }
                }
                base = base->tp_base;
            } else {
                break;
            }
        }
    }

    // Instance-dict visit (mirror of subtype_traverse's dictoffset block).
    // The saved "original" for a private-attribute class is subtype_traverse,
    // but because the class's LIVE tp_traverse has been replaced with this
    // wrapper, subtype_traverse skips its own slot-walking loop, keeps base ==
    // typ, and its `type->tp_dictoffset != base->tp_dictoffset` check sees equal
    // offsets -> it never visits the instance __dict__. The instance __dict__
    // (e.g. `self.__dict__['x'] = self` self-cycles) would then be invisible to
    // the collector and leak. Do the visit explicitly here instead.
    //
    // The managed-dict API is version-dependent: 3.13+ exposes the public
    // PyObject_VisitManagedDict, 3.12 only the internal _PyObject_VisitManagedDict,
    // and <=3.11 has no managed dict at all (classic positive tp_dictoffset,
    // visited through _PyObject_GetDictPtr like subtype_traverse does).
#if PY_VERSION_HEX >= 0x030D0000
    if ((typ->tp_flags & Py_TPFLAGS_MANAGED_DICT)
        && typ->tp_dictoffset == -1) {
        int err = PyObject_VisitManagedDict(self, visit, arg);
        if (err) {
            return err;
        }
    }
#elif PY_VERSION_HEX >= 0x030C0000
    if ((typ->tp_flags & Py_TPFLAGS_MANAGED_DICT)
        && typ->tp_dictoffset == -1) {
        int err = _PyObject_VisitManagedDict(self, visit, arg);
        if (err) {
            return err;
        }
    }
#else
    PyObject** dictptr = _PyObject_GetDictPtr(self);
    if (dictptr && *dictptr) {
        int err = visit(*dictptr, arg);
        if (err) {
            return err;
        }
    }
#endif

    // Instance->type link (mirror of subtype_traverse's heap-type visit).
    // subtype_traverse(self) does Py_VISIT(Py_TYPE(self)) only when the
    // resolved base traverse does NOT belong to a heap type. Because the
    // instance's LIVE tp_traverse has been replaced with this wrapper,
    // subtype_traverse keeps base == typ (heap), so that Py_VISIT is skipped
    // and a cycle like  A ->(tp_dict)-> a  and  a ->(ob_type)-> A  is invisible
    // to the collector. Visit the instance's type explicitly when it is a heap
    // type, matching subtype_traverse's intent. (This is the same fix applied
    // to the metaclass wrapper PrivateAttrType_tp_traverse.)
    {
        PyTypeObject* meta = Py_TYPE(self);
        if (meta && (meta->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
            Py_VISIT((PyObject*)meta);
        }
    }

    // traverse per-object private attributes for this type and its parents
    std::vector<uintptr_t> parent_ids;
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()){
        parent_ids = ::AllData::all_type_parent_id[typ_id];
    }
    // include typ_id itself
    parent_ids.insert(parent_ids.begin(), typ_id);

    for (auto pid : parent_ids) {
        auto it = ::AllData::all_object_attr.find(pid);
        if (it == ::AllData::all_object_attr.end()) continue;
        auto it2 = it->second.find(id_self);
        if (it2 == it->second.end()) continue;
        auto &attrmap = it2->second;
        for (auto &p : attrmap) {
            PyObject* obj = p.second.get();
            if (obj) {
                int res = visit(obj, arg);
                if (res) return res;
            }
        }
    }

    return 0;
}

static int
PrivateAttr_tp_clear(PyObject* self) noexcept
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t typ_id = (uintptr_t)typ;
    uintptr_t id_self = (uintptr_t)self;

    // Re-entrancy guard: see PrivateAttrRecursionGuard above.
    if (!PrivateAttrRecursionGuard::try_enter(id_self)) {
        return 0;
    }
    PrivateAttrRecursionGuard guard(id_self);

    // call original clear if exists
    if (::AllData::all_type_clear.find(typ_id) != ::AllData::all_type_clear.end()) {
        inquiry orig = ::AllData::all_type_clear[typ_id];
        if (orig) {
            orig(self);
        }
    } else {
        inquiry orig = get_need_tp_clear(typ);
        if (orig) orig(self);
    }

    // __slots__ clear (mirror of subtype_clear's clear_slots loop). Same
    // reasoning as the traverse side: subtype_clear can't run its own loop
    // once the LIVE tp_clear is this wrapper, so clear the __slots__ members
    // of this type and every delegating base explicitly.
    {
        PyTypeObject* base = typ;
        while (base) {
            if (base->tp_clear == ::AllData::captured_subtype_clear
                || base->tp_clear == PrivateAttr_tp_clear) {
                if (Py_SIZE(base)) {
                    PrivateAttr_clear_slots(base, self);
                }
                base = base->tp_base;
            } else {
                break;
            }
        }
    }

    // Instance-dict clear (mirror of subtype_clear's dict handling). Same
    // reasoning as the traverse side: subtype_clear can't run its dict block
    // once the LIVE tp_clear has been replaced, so clear the instance dict
    // explicitly to break __dict__ self-cycles. Version-dependent APIs, see
    // the matching traverse block above.
#if PY_VERSION_HEX >= 0x030D0000
    if ((typ->tp_flags & Py_TPFLAGS_MANAGED_DICT)
        && typ->tp_dictoffset == -1) {
        PyObject_ClearManagedDict(self);
    }
#elif PY_VERSION_HEX >= 0x030C0000
    if ((typ->tp_flags & Py_TPFLAGS_MANAGED_DICT)
        && typ->tp_dictoffset == -1) {
        _PyObject_ClearManagedDict(self);
    }
#else
    PyObject** dictptr = _PyObject_GetDictPtr(self);
    if (dictptr && *dictptr) {
        Py_CLEAR(*dictptr);
    }
#endif

    // clear our per-object data
    clear_object_related(id_self, typ_id);
    return 0;
}

class KeepPythonException {
private:
#if PY_VERSION_HEX < 0x030C0000
    PyObject* typ = NULL;
    PyObject* tb = NULL;
#endif
    PyObject* exc = NULL;
public:
    KeepPythonException() noexcept {
        if (PyErr_Occurred())
        // under 3.12 use PyErr_Fetch
#if PY_VERSION_HEX < 0x030C0000
        PyErr_Fetch(&typ, &exc, &tb);
#else
        exc = PyErr_GetRaisedException();
#endif
    }

    ~KeepPythonException() noexcept {
        if (exc)
        // under 3.12 use PyErr_Restore
#if PY_VERSION_HEX < 0x030C0000
        PyErr_Restore(typ, exc, tb);
#else
        PyErr_SetRaisedException(exc);
#endif
    }
};

static traverseproc get_type_need_tp_traverse(PyTypeObject* cls) noexcept;
static inquiry get_type_need_tp_clear(PyTypeObject* cls) noexcept;

// metaclass-level (type object) traverse/clear: call original if any, and handle AllData-stored type attributes
static int
PrivateAttrType_tp_traverse(PyObject* self, visitproc visit, void* arg) noexcept
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t typ_id = (uintptr_t)self;

    // Re-entrancy guard: see PrivateAttrRecursionGuard above.
    if (!PrivateAttrRecursionGuard::try_enter((uintptr_t)self)) {
        return 0;
    }
    PrivateAttrRecursionGuard guard((uintptr_t)self);

    // Recover the original traverse via get_type_need_tp_traverse, skipping
    // any delegating default (subtype_traverse / PrivateAttr_tp_traverse / our
    // own wrapper). For a type object the only correct "real" traverse is
    // CPython's type_traverse; when no real one exists (plain class objects
    // delegate through subtype_traverse, which would follow the LIVE tp_traverse
    // slot straight back into this wrapper), we rely on the AllData traversal
    // below to reach the class's hard self-cycle (tp_dict / tp_mro / tp_bases)
    // through the type-level private attributes we visit.
    {
        traverseproc orig = get_type_need_tp_traverse(typ);
        if (orig) {
            int res = orig(self, visit, arg);
            if (res) return res;
        }
    }

    // Mirror CPython's subtype_traverse for the instance->type link.
    // subtype_traverse(self) does Py_VISIT(Py_TYPE(self)) when the instance is
    // a heap type and the resolved base traverse does not belong to a heap
    // type, so cycles between a class object and its (heap) metaclass are
    // visible to the collector. type_traverse alone does NOT visit ob_type:
    // it only walks tp_dict / tp_mro / tp_bases, so without this step a cycle
    // like  M ->(dict attr)-> C  and  C ->(ob_type)-> M  is invisible to GC
    // and leaks. self here is a TYPE object; its Py_TYPE is the metaclass
    // (which is a heap type when registered/created by type()). We only emit
    // the visit when the metaclass is a heap type, matching subtype_traverse's
    // condition and avoiding the trivial/static type link.
    {
        if (typ->tp_flags & Py_TPFLAGS_HEAPTYPE) {
            Py_VISIT((PyObject*)typ);
        }
    }

    // Traverse PyObjects stored in AllData for this type:
    // - direct type attributes ::AllData::type_attr_dict[typ_id]
    // - attributes stored under parents for this type ::AllData::all_type_subclass_attr[parent_id][typ_id]
    std::vector<uintptr_t> parent_ids;
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()) {
        parent_ids = ::AllData::all_type_parent_id[typ_id];
    }
    // include typ_id itself first
    parent_ids.insert(parent_ids.begin(), typ_id);

    for (auto pid : parent_ids) {
        if (pid == typ_id) {
            if (::AllData::type_attr_dict.find(pid) != ::AllData::type_attr_dict.end()) {
                auto &item_set = ::AllData::type_attr_dict[pid];
                if (::AllData::all_type_mutex.find(pid) != ::AllData::all_type_mutex.end()) {
                    std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[pid]);
                    for (auto &p : item_set) {
                        PyObject* obj = p.second.get();
                        if (obj) {
                            int res = visit(obj, arg);
                            if (res) return res;
                        }
                    }
                } else {
                    for (auto &p : item_set) {
                        PyObject* obj = p.second.get();
                        if (obj) {
                            int res = visit(obj, arg);
                            if (res) return res;
                        }
                    }
                }
            }
        } else {
            if (::AllData::all_type_subclass_attr.find(pid) == ::AllData::all_type_subclass_attr.end()) continue;
            auto &child_map = ::AllData::all_type_subclass_attr[pid];
            auto it = child_map.find(typ_id);
            if (it == child_map.end()) continue;
            if (::AllData::all_type_subclass_mutex.find(pid) != ::AllData::all_type_subclass_mutex.end()
                && ::AllData::all_type_subclass_mutex[pid].find(typ_id) != ::AllData::all_type_subclass_mutex[pid].end()) {
                std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[pid][typ_id]);
                for (auto &p : it->second) {
                    PyObject* obj = p.second.get();
                    if (obj) {
                        int res = visit(obj, arg);
                        if (res) return res;
                    }
                }
            } else {
                for (auto &p : it->second) {
                    PyObject* obj = p.second.get();
                    if (obj) {
                        int res = visit(obj, arg);
                        if (res) return res;
                    }
                }
            }
        }
    }

    return 0;
}

static int
PrivateAttrType_tp_clear(PyObject* self) noexcept
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t typ_id = (uintptr_t)self;

    // Re-entrancy guard: see PrivateAttrRecursionGuard above.
    if (!PrivateAttrRecursionGuard::try_enter((uintptr_t)self)) {
        return 0;
    }
    PrivateAttrRecursionGuard guard((uintptr_t)self);

    // Recover the original clear via get_type_need_tp_clear, skipping any
    // delegating default (subtype_clear / PrivateAttr_tp_clear / our own
    // wrapper). subtype_clear on a type object would clear the class's own
    // tp_dict (its namespace), which must never happen here; when the walker
    // finds no real clear, the AllData cleanup below still breaks the cycle by
    // clearing the type-level private attributes we stored (their PyObjectStorage
    // DECREFs release the references that keep the class's tp_dict / tp_mro /
    // ht_module alive), after which CPython's own type_clear runs on the class
    // once the collector re-visits it.
    {
        inquiry orig = get_type_need_tp_clear(typ);
        if (orig) {
            int result = orig(self);
            if (result) return result;
        }
    }

    // Clear AllData entries associated with this type. PyObjectStorage destructor
    // takes care of DECREF for stored objects.
    std::vector<uintptr_t> parent_ids;
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()) {
        parent_ids = ::AllData::all_type_parent_id[typ_id];
    }

    // Clear direct type attributes
    if (::AllData::type_attr_dict.find(typ_id) != ::AllData::type_attr_dict.end()) {
        ::AllData::type_attr_dict.erase(typ_id);
    }

    // Clear subclass attribute entries stored under parent types for this type
    for (auto parent_id : parent_ids) {
        if (::AllData::all_type_subclass_attr.find(parent_id) != ::AllData::all_type_subclass_attr.end()) {
            auto &child_map = ::AllData::all_type_subclass_attr[parent_id];
            if (child_map.find(typ_id) != child_map.end()) {
                child_map.erase(typ_id);
            }
        }
        if (::AllData::all_type_subclass_mutex.find(parent_id) != ::AllData::all_type_subclass_mutex.end()) {
            auto &mmap = ::AllData::all_type_subclass_mutex[parent_id];
            if (mmap.find(typ_id) != mmap.end()) {
                mmap.erase(typ_id);
            }
        }
    }

    return 0;
}

static void
PrivateAttr_object_init_private_dict(uintptr_t obj_id, uintptr_t type_id) noexcept
{
    object_create_mutex(type_id, obj_id);
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[type_id][obj_id]);
        if (::AllData::all_object_attr[type_id].find(obj_id) == ::AllData::all_object_attr[type_id].end()) {
            ::AllData::all_object_attr[type_id][obj_id] = {};
        }
    }
    if (::AllData::all_type_parent_id.find(type_id) != ::AllData::all_type_parent_id.end()) {
        for (auto parent_id : ::AllData::all_type_parent_id[type_id]) {
            PrivateAttr_object_init_private_dict(obj_id, parent_id);
        }
    }
}

static PyObject* PrivateAttrType_new(PyTypeObject* type, PyObject* args, PyObject* kwds) noexcept;
static PyObject* PrivateAttrType_getattr(PyObject* cls, PyObject* name) noexcept;
static int PrivateAttrType_setattr(PyObject* cls, PyObject* name, PyObject* value) noexcept;
static PyObject* PrivateAttrType_call(PyObject* cls, PyObject* args, PyObject* kwds) noexcept;
static void PrivateAttrType_del(PyObject* cls) noexcept;

static PyTypeObject PrivateAttrType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute.PrivateAttrType",        // tp_name
    sizeof(PrivateAttrTypeObject),              // tp_basicsize
    0,                                          // tp_itemsize
    (destructor)PrivateAttrType_del,            // tp_dealloc
    0,                                          // tp_print
    0,                                          // tp_getattr
    0,                                          // tp_setattr
    0,                                          // tp_reserved
    0,                                          // tp_repr
    0,                                          // tp_as_number
    0,                                          // tp_as_sequence
    0,                                          // tp_as_mapping
    0,                                          // tp_hash
    (ternaryfunc)PrivateAttrType_call,          // tp_call
    0,                                          // tp_str
    (getattrofunc)PrivateAttrType_getattr,      // tp_getattro
    (setattrofunc)PrivateAttrType_setattr,      // tp_setattro
    0,                                          // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE
    | Py_TPFLAGS_HAVE_GC,                       // tp_flags
    "metaclass for private attributes",         // tp_doc
    PrivateAttrType_tp_traverse,                // tp_travers
    PrivateAttrType_tp_clear,                   // tp_clear
    0,                                          // tp_richcompare
    0,                                          // tp_weaklistoffset
    0,                                          // tp_iter
    0,                                          // tp_iternext
    0,                                          // tp_methods
    0,                                          // tp_members
    0,                                          // tp_getset
    &PyType_Type,                               // tp_base
    0,                                          // tp_dict
    0,                                          // tp_descr_get
    0,                                          // tp_descr_set
    0,                                          // tp_dictoffset
    0,                                          // tp_init
    0,                                          // tp_alloc
    (newfunc)PrivateAttrType_new,               // tp_new
};

static bool
is_private_meta_type(PyObject* typ) noexcept
{
    return PyObject_IsSubclass(typ, (PyObject*)&PrivateAttrType) || is_registered_type(typ);
}

static PyObject*
get_string_hash_tuple(const std::string& name) noexcept
{
    std::string name1;
    std::string name2;
    name1 = module_running_time_string + "_" + name;
    uintptr_t type_id = reinterpret_cast<uintptr_t>(&PrivateAttrType);
    name2 = std::to_string(type_id) + "_" + compile_time + "_" + name1;
    std::string name1hash, name2hash;
    picosha2::hash256_hex_string(name1, name1hash);
    picosha2::hash256_hex_string(name2, name2hash);
    return Py_BuildValue("(ss)", name1hash.c_str(), name2hash.c_str());
}

static TwoStringTuple
get_string_hash_tuple2(const std::string& name) noexcept
{
    std::string name1;
    std::string name2;
    name1 = module_running_time_string + "_" + name;
    uintptr_t type_id = reinterpret_cast<uintptr_t>(&PrivateAttrType);
    name2 = std::to_string(type_id) + "_" + compile_time + "_" + name1;
    std::string name1hash, name2hash;
    picosha2::hash256_hex_string(name1, name1hash);
    picosha2::hash256_hex_string(name2, name2hash);
    return TwoStringTuple(name1hash, name2hash);
}

static FinalObject
type_get_final_attr(uintptr_t main_type, uintptr_t target_type, const std::string& name) noexcept
{
    // main_type is the code owner (the class declaring the attribute private);
    // target_type is the object's type whose attribute is being accessed. Only
    // type_attr_dict[main_type] and all_type_subclass_attr[main_type][*] are
    // analysed. Each visited type's own type_need_call drives its key.
    std::vector<uintptr_t> chain;
    chain.push_back(target_type);
    if (::AllData::all_type_parent_id.find(target_type) != ::AllData::all_type_parent_id.end()) {
        auto& parent_ids = ::AllData::all_type_parent_id[target_type];
        chain.insert(chain.end(), parent_ids.begin(), parent_ids.end());
    }
    for (auto& type : chain) {
        std::string key;
        PyObject* need_call = get_type_need_call(type);
        if (need_call != NULL) {
            auto private_name_result = custom_random_string(type, name, need_call);
            if (!private_name_result.ok) {
                return -2; // -2 means exception
            }
            key = private_name_result.value;
        } else {
            key = default_random_string(type, name);
        }
        if (type == main_type) {
            type_create_mutex(main_type);
            std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[main_type]);
            auto& item_set = ::AllData::type_attr_dict[main_type];
            if (item_set.find(key) != item_set.end()) {
                PyObject* obj = item_set[key];
                return obj;
            } else if (::AllData::type_creation_locked && ::AllData::all_type_attr_set.find(main_type) == ::AllData::all_type_attr_set.end()) {
                // now we can only use name instead of key on tmp_creation_data->need_remove_itself[name]
                if (::AllData::tmp_creation_data &&
                    ::AllData::tmp_creation_data->need_remove_itself.find(name) != ::AllData::tmp_creation_data->need_remove_itself.end()) {
                    PyObject* obj = ::AllData::tmp_creation_data->need_remove_itself[name];
                    return obj;
                }
            }
            return -1; // main_type has no such attribute -> not found
        }
        if (::AllData::all_type_subclass_attr.find(main_type) != ::AllData::all_type_subclass_attr.end()) {
            auto& now_mro_dict = ::AllData::all_type_subclass_attr[main_type];
            if (now_mro_dict.find(type) != now_mro_dict.end()) {
                type_parenttype_create_mutex(main_type, type);
                std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[main_type][type]);
                if (now_mro_dict[type].find(key) != now_mro_dict[type].end()) {
                    PyObject* obj = now_mro_dict[type][key];
                    return obj;
                }
            }
        }
    }
    return -1; // -1 means not found
}

static AnalyseStatus
need_analyse_type(PyObject* type) noexcept;

static ParentIdsResult
type_get_parent_ids(uintptr_t type_id) noexcept
{
    auto it = ::AllData::all_type_parent_id.find(type_id);
    if (it != ::AllData::all_type_parent_id.end()) {
        return {true, &it->second};
    }
    // The type has not been fully registered yet (e.g. inside __init_subclass__):
    // build the MRO chain on the fly from tp_mro.
    std::vector<uintptr_t> mro_vector;
    PyTypeObject* tp = (PyTypeObject*)type_id;
    if (PyType_Check(tp) && tp->tp_mro) {
        Py_ssize_t mro_size = PyTuple_GET_SIZE(tp->tp_mro);
        for (Py_ssize_t i = 1; i < mro_size; i++) {
            PyObject* item = PyTuple_GET_ITEM(tp->tp_mro, i);
            if (!item || !PyType_Check(item)) {
                continue;
            }
            AnalyseStatus analyse_status = need_analyse_type(item);
            if (analyse_status == AnalyseStatus::Error) {
                return {false, nullptr};
            }
            if (analyse_status == AnalyseStatus::No) {
                continue;
            }
            mro_vector.push_back((uintptr_t)item);
        }
    }
    ::AllData::all_type_parent_id[type_id] = mro_vector;
    return {true, &::AllData::all_type_parent_id[type_id]};
}

static TypePrivateAttrResult
type_private_attr(uintptr_t type_id, const std::string& name) noexcept
{
    TwoStringTuple hash_tuple = get_string_hash_tuple2(name);
    if (::AllData::all_type_attr_set.find(type_id) != ::AllData::all_type_attr_set.end()) {
        auto& item_set = ::AllData::all_type_attr_set[type_id];
        if (item_set.find(hash_tuple) != item_set.end()) {
            return {true, true};
        }
    } else if (::AllData::type_creation_locked && ::AllData::all_type_attr_set.find(type_id) == ::AllData::all_type_attr_set.end()) {
        if (::AllData::tmp_creation_data &&
            ::AllData::tmp_creation_data->private_attrs_set.find(hash_tuple) != ::AllData::tmp_creation_data->private_attrs_set.end()) {
            return {true, true};
        }
    }
    ParentIdsResult parent_ids = type_get_parent_ids(type_id);
    if (!parent_ids.ok) {
        return {false, false};
    }
    if (parent_ids.ids) {
        for (auto& parent_id: *parent_ids.ids) {
            if (::AllData::all_type_attr_set.find(parent_id) != ::AllData::all_type_attr_set.end()) {
                auto& item_set = ::AllData::all_type_attr_set[parent_id];
                if (item_set.find(hash_tuple) != item_set.end()) {
                    return {true, true};
                }
            }
        }
    }
    return {true, false};
}

static PyCodeObject*
get_now_code() noexcept
{
    PyFrameObject* f = PyEval_GetFrame();
    if (!f) {
        return NULL;
    }
    PyCodeObject* code = PyFrame_GetCode(f);
    return code;
}

static PyObject*
try_get_attr_string(PyObject* obj, const char* name) noexcept
{
    if (obj == NULL || name == NULL) {
        return NULL;
    }
    if (PyObject_HasAttrString(obj, name) <= 0) {
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        return NULL;
    }
    return PyObject_GetAttrString(obj, name);
}

static void
analyse_all_code(PyObject* obj, std::unordered_map<uintptr_t, PyObjectStorage>& map, std::unordered_set<uintptr_t>& _seen) noexcept
{
    uintptr_t obj_id = (uintptr_t)obj;
    if (_seen.find(obj_id) != _seen.end()) {
        return;
    }
    _seen.insert(obj_id);
    if (PyObject_TypeCheck(obj, &PyCode_Type)) {
        map[(uintptr_t)obj] = obj;
        PyObjectStorage co_contain = PyObjectStorage::from_new_ref(try_get_attr_string(obj, "co_consts"));
        if (co_contain && PySequence_Check(co_contain)) {
            Py_ssize_t len = PySequence_Length(co_contain);
            if (len >= 0) {
                for (Py_ssize_t i = 0; i < len; i++) {
                    PyObjectStorage item = PyObjectStorage::from_new_ref(PySequence_GetItem(co_contain, i));
                    if (item) {
                        analyse_all_code(item, map, _seen);
                    }
                }
            }
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PrivateWrapType)) {
        PyObject* func_list = ((PrivateWrapObject*)obj)->func_list;
        if (func_list && PySequence_Check(func_list)) {
            Py_ssize_t len = PySequence_Length(func_list);
            if (len >= 0) {
                for (Py_ssize_t i = 0; i < len; i++) {
                    PyObjectStorage func = PyObjectStorage::from_new_ref(PySequence_GetItem(func_list, i));
                    if (func) {
                        analyse_all_code(func, map, _seen);
                    }
                }
            }
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PrivateAttrSlotType) ||
        PyObject_TypeCheck(obj, &PrivateAttrMetaSlotType) ||
        PyObject_TypeCheck(obj, &PrivateAttrMetaNewSlotType)) {
        PrivateAttrSlot* slot = (PrivateAttrSlot*)obj;
        if (slot->callback != NULL) {
            analyse_all_code(slot->callback, map, _seen);
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PyProperty_Type)) {
        PyObjectStorage fget = PyObjectStorage::from_new_ref(try_get_attr_string(obj, "fget"));
        if (fget) {
            analyse_all_code(fget, map, _seen);
        }
        PyObjectStorage fset = PyObjectStorage::from_new_ref(try_get_attr_string(obj, "fset"));
        if (fset) {
            analyse_all_code(fset, map, _seen);
        }
        PyObject* fdel = PyObjectStorage::from_new_ref(try_get_attr_string(obj, "fdel"));
        if (fdel) {
            analyse_all_code(fdel, map, _seen);
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PyClassMethod_Type) || PyObject_TypeCheck(obj, &PyStaticMethod_Type)) {
        PyObjectStorage func = PyObjectStorage::from_new_ref(try_get_attr_string(obj, "__func__"));
        if (func) {
            analyse_all_code(func, map, _seen);
        }
        return;
    }
    {
        PyObjectStorage wrap = PyObjectStorage::from_new_ref(try_get_attr_string(obj, "__wrapped__"));
        if (wrap) {
            if (wrap == obj) {
                if (PyObject_TypeCheck(obj, &PyFunction_Type)) {
                    PyObjectStorage code = PyFunction_GetCode(obj);
                    if (code) {
                        analyse_all_code(code, map, _seen);
                    }
                    return;
                }
            }
            analyse_all_code(wrap, map, _seen);
            return;
        }
    }
    if (PyObject_TypeCheck(obj, &PyFunction_Type)) {
        PyObjectStorage code = PyFunction_GetCode(obj);
        if (code) {
            analyse_all_code(code, map, _seen);
        }
        return;
    }
}

static std::string
real_class_name(const std::string& name, const std::string& class_name) noexcept
{
    size_t start_pos = class_name.find_first_not_of('_');
    std::string cleaned_class_name = (start_pos == std::string::npos) ? "" : class_name.substr(start_pos);
    std::string pre_underline = cleaned_class_name.size() > 0 ? "_" : "";

    // if the name starts with "__" but does not end with "__", change to _ClassName__name
    if (name.length() >= 2 && name.substr(0, 2) == "__" && name.substr(name.length() - 2) != "__") {
        return pre_underline + cleaned_class_name + name;
    }
    return name;
}

static AnalyseStatus
need_analyse_type(PyObject* type) noexcept
{
    // Fast path: reuse a previously computed, still-valid answer. The entry is
    // kept alive (and removed) together with the type object via a weakref, so
    // a cached result can never be served for a type that has been collected.
    analyse_cache_lookup_result cached = find_is_private_class(type);
    if (cached.found) {
        return cached.result ? AnalyseStatus::Yes : AnalyseStatus::No;
    }

    bool result = false;

    // (1) Types created through PrivateAttrType always need analysis.
    int is_private_attr = PyObject_IsInstance(type, (PyObject*)&PrivateAttrType);
    if (is_private_attr < 0) {
        return AnalyseStatus::Error;
    }
    if (is_private_attr) {
        result = true;
    } else {
        // (2) Types whose metaclass is a registered metaclass need analysis.
        std::shared_lock lock(::AllData::all_register_new_metaclass_mutex);
        for (auto& [id, metaclassref]: ::AllData::all_register_type_weak_ref){
            if (!PyWeakref_CheckRef((PyObject*)metaclassref)) {
                continue;
            }
#if PY_VERSION_HEX < 0x030D0000
            PyObjectStorage metaclass = PyWeakref_GET_OBJECT((PyObject*)metaclassref);
            // A cleared weakref reports Py_None as its referent.
            if (!metaclass.get() || metaclass.get() == Py_None || type == metaclass.get()) {
                continue;
            }
            int is_instance = PyObject_IsInstance(type, metaclass.get());
            if (is_instance < 0) {
                return AnalyseStatus::Error;
            }
            if (is_instance) {
                result = true;
                break;
            }
#else
            PyObjectStorage metaclass;
            if (PyWeakref_GetRef(metaclassref, (metaclass.getpointer())) == 1) {
                if (type == metaclass.get()) {
                    continue;
                }
                int is_instance = PyObject_IsInstance(type, metaclass.get());
                if (is_instance < 0) {
                    return AnalyseStatus::Error;
                }
                if (is_instance) {
                    result = true;
                    break;
                }
            }
#endif
        }
    }

    if (store_cache_for_is_private_class(type, result) < 0) {
        return AnalyseStatus::Error;
    }

    return result ? AnalyseStatus::Yes : AnalyseStatus::No;
}

static bool
PrivateAttrType_preprocess(PyObject* args, PyObject* kwds, PrivateAttrCreationData& data, const std::string& func_name) noexcept
{
    static const char* invalid_name[] = {"__private_attrs__", "__slots__", "__getattribute__", "__getattr__", "__init__",
        "__setattr__", "__delattr__", "__name__", "__module__", "__doc__", "__getstate__", "__setstate__",
        "__get__", "__set__", "__delete__", "__new__", "__set_name__", "__class__", "__call__", NULL};

    if (!args) {
        PyErr_Format(PyExc_SystemError, "%s (%d): args is NULL", __FILE__, __LINE__);
        return false;
    }

    // only parse name, bases, attrs
    std::string parse = "OOO";
    if (func_name.length() > 0) {
        parse += ":" + func_name;
    }
    PyObject *name, *bases, *attrs;
    if (!PyArg_ParseTuple(args, parse.c_str(), &name, &bases, &attrs)) {
        return false;
    }
    data.name = name;
    data.bases = bases;

    if (!PyUnicode_Check(data.name)) {
        PyErr_SetString(PyExc_TypeError, "name must be a string");
        return false;
    }
    data.class_name = PyUnicode_AsUTF8(data.name);

    if (!PyTuple_Check(data.bases)) {
        PyErr_SetString(PyExc_TypeError, "bases must be a tuple");
        return false;
    }

    if (!PyAnyDict_Check(attrs)) {
        PyErr_SetString(PyExc_TypeError, "attrs must be a dict");
        return false;
    }

    if (kwds && PyDict_Check(kwds)) {
        int has_use_parents = PyDict_ContainsString(kwds, "use_parents");
        if (has_use_parents < 0) {
            return false;
        }
        if (has_use_parents) {
            PyObject* use_parents_obj = PyDict_GetItemString(kwds, "use_parents");
            int truth = PyObject_IsTrue(use_parents_obj);
            if (truth < 0) {
                return false;
            }
            data.use_parents = (truth == 1);
        }
    }

    PyObjectStorage __private_attrs__;
    int contains_private_attrs = PyDict_ContainsString(attrs, "__private_attrs__");
    if (contains_private_attrs < 0) {
        return false;
    }
    if (contains_private_attrs) {
        __private_attrs__ = PyDict_GetItemString(attrs, "__private_attrs__");
    }
    else {
        __private_attrs__ = PyObjectStorage::from_new_ref(PyTuple_New(0));
    }

    if (!__private_attrs__) {
        return false;
    }

    if (PyUnicode_Check(__private_attrs__)) {
        PyObject* new___private_attrs__ = PyTuple_New(1);
        if (!new___private_attrs__) {
            return false;
        }
        PyTuple_SET_ITEM(new___private_attrs__, 0, __private_attrs__);
        __private_attrs__ = PyObjectStorage::from_new_ref(new___private_attrs__);
    }

    if (!PySequence_Check(__private_attrs__)) {
        PyErr_SetString(PyExc_TypeError, "'__private_attrs__' must be a sequence");
        return false;
    }

    data.attrs_copy = PyObjectStorage::from_new_ref(PyDict_Copy(attrs));
    if (!data.attrs_copy) {
        return false;
    }

    Py_ssize_t private_attr_len = PySequence_Length(__private_attrs__);
    if (private_attr_len < 0) {
        return false;
    }
    PyObjectStorage new_hash_private_attrs = PyObjectStorage::from_new_ref(PyTuple_New(private_attr_len));
    if (!new_hash_private_attrs) {
        return false;
    }

    for (Py_ssize_t i = 0; i < private_attr_len; i++) {
        PyObject* attr = PySequence_GetItem(__private_attrs__, i);
        if (!attr) {
            return false;
        }

        if (!PyUnicode_Check(attr)) {
            PyErr_Format(PyExc_TypeError, "all items in '__private_attrs__' must be 'str', not '%.200s'", Py_TYPE(attr)->tp_name);
            return false;
        }

        const char* attr_cstr = PyUnicode_AsUTF8(attr);
        if (!attr_cstr) {
            return false;
        }

        std::string attr_str = real_class_name(attr_cstr, data.class_name);

        for (const char** p = invalid_name; *p != NULL; p++) {
            if (strcmp(attr_str.c_str(), *p) == 0) {
                PyErr_Format(PyExc_TypeError, "Invalid name '%s' in '__private_attrs__'", attr_str.c_str());
                return false;
            }
        }

        PyObject* hash_tuple = get_string_hash_tuple(attr_str);
        TwoStringTuple hash_tuple_key = get_string_hash_tuple2(attr_str);
        if (!hash_tuple) {
            return false;
        }
        PyTuple_SET_ITEM(new_hash_private_attrs.get(), i, hash_tuple);
        data.private_attrs_set.insert(hash_tuple_key);
        data.private_attrs_vector_string.insert(attr_str);
    }

    if (PyDict_SetItemString(data.attrs_copy, "__private_attrs__", new_hash_private_attrs) < 0) {
        return false;
    }

    int has_slots = PyDict_ContainsString(data.attrs_copy, "__slots__");
    if (has_slots < 0) {
        return false;
    }

    if (has_slots) {
        PyObject* all_slots = PyDict_GetItemString(data.attrs_copy, "__slots__");
        if (!all_slots) {return false;}
        if (PyUnicode_Check(all_slots)) {
            const char* slot_cstr = PyUnicode_AsUTF8(all_slots);
            if (data.private_attrs_vector_string.find((std::string)slot_cstr) != data.private_attrs_vector_string.end()){
                PyErr_Format(PyExc_TypeError, "'__slots__' and '__private_attrs__' cannot have the same attribute name: '%s'", slot_cstr);
                return false;
            }
        } else {
            PyObjectStorage slot_seq;
            if (PyAnyDict_Check(all_slots)) {
                // use the key of the dict as the slot name
                PyObject* keys = PyDict_Keys(all_slots);
                if (!keys) {
                    return false;
                }
                slot_seq = PyObjectStorage::from_new_ref(keys);
            } else {
                slot_seq = PyObjectStorage::from_new_ref(PySequence_Fast(all_slots, "__slots__ must be a string, dict or sequence"));
            }
            if (!slot_seq.get()) {
                return false;
            }

            Py_ssize_t slot_len = PySequence_Fast_GET_SIZE(slot_seq.get());

            for (Py_ssize_t j = 0; j < slot_len; j++) {
                PyObject* slot = PySequence_Fast_GET_ITEM(slot_seq.get(), j);
                if (PyUnicode_Check(slot)) {
                    const char* slot_cstr = PyUnicode_AsUTF8(slot);
                    std::string final_name = real_class_name(slot_cstr, data.class_name);
                    if (data.private_attrs_vector_string.find(final_name) != data.private_attrs_vector_string.end()){
                        PyErr_Format(PyExc_TypeError, "'__slots__' and '__private_attrs__' cannot have the same attribute name: '%s'", final_name.c_str());
                        return false;
                    }
                }
            }
        }
    }

    Py_ssize_t bases_len = PyTuple_GET_SIZE(data.bases.get());
    for (Py_ssize_t i = 0; i < bases_len; i++) {
        PyObject* base = PyTuple_GET_ITEM(data.bases.get(), i);
        if (!base || !PyType_Check(base)) {
            continue;
        }
        AnalyseStatus analyse_status = need_analyse_type(base);
        if (analyse_status == AnalyseStatus::Error) {
            return false;
        }
        if (analyse_status == AnalyseStatus::No) {
            continue;
        }
        data.all_need_analyse_base.push_back((uintptr_t)base);
    }

    if (data.use_parents) {
        // 2.2.0: when use_parents=True, merge every ancestor's private attribute
        // names into this class's own private-attr set (the TwoStringTuple set).
        // The Python-level '__private_attrs__' tuple is left untouched; only
        // AllData::all_type_attr_set[this] gains the inherited names.
        std::vector<uintptr_t> all_ancestors;
        for (auto& base : data.all_need_analyse_base) {
            all_ancestors.push_back(base);
            if (::AllData::all_type_parent_id.find(base) != ::AllData::all_type_parent_id.end()) {
                auto& ancestor_ids = ::AllData::all_type_parent_id[base];
                all_ancestors.insert(all_ancestors.end(), ancestor_ids.begin(), ancestor_ids.end());
            }
        }
        for (auto& ancestor : all_ancestors) {
            if (::AllData::all_type_attr_set.find(ancestor) != ::AllData::all_type_attr_set.end()) {
                auto& ancestor_set = ::AllData::all_type_attr_set[ancestor];
                data.private_attrs_set.insert(ancestor_set.begin(), ancestor_set.end());
            }
        }
    }

    std::function<NeedRemoveResult(const std::string&)> need_remove_to_subclass = [&data](const std::string& attr_name){
        for (auto& base: data.all_need_analyse_base) {
            TypePrivateAttrResult private_result = type_private_attr(base, attr_name);
            if (!private_result.ok) {
                return NeedRemoveResult{false, 0};
            }
            if (private_result.is_private) {
                return NeedRemoveResult{true, base};
            }
        }
        return NeedRemoveResult{true, 0};
    };

    // forbidden to have "__static_attributes__" in attrs, because it exposes the private attributes name.
    int has_static_attributes = PyDict_ContainsString(data.attrs_copy, "__static_attributes__");
    if (has_static_attributes < 0) {
        return false;
    }
    if (has_static_attributes) {
        if (PyDict_DelItemString(data.attrs_copy, "__static_attributes__") < 0) return false;
    }

    {
        Py_ssize_t pos = 0;
        PyObject* key, *value;
        PyObjectStorage forward_analyse = PyObjectStorage::from_new_ref(PyDict_Copy(data.attrs_copy));
        while (PyDict_Next(forward_analyse, &pos, &key, &value)) {
            if (!key || !PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError, "all keys in 'attrs' must be strings");
                return false;
            }
            std::string attr_name = real_class_name(PyUnicode_AsUTF8(key), data.class_name);
            PyObject* need_value;
            if (PyObject_IsInstance(value, (PyObject*)&PrivateWrapType)) {
                need_value = ((PrivateWrapObject*)value)->result;
            } else {
                need_value = value;
            }
            if (data.private_attrs_vector_string.find(attr_name) != data.private_attrs_vector_string.end()) {
                // PyObjectStorage assignment INCREFs the value (RAII); the
                // reference is released when need_remove_itself is cleared
                // (PrivateAttrCreationData::clear). No manual INCREF here,
                // otherwise the value leaks (+1) when the class is destroyed.
                data.need_remove_itself[attr_name] = need_value;
                if (PyDict_DelItem(data.attrs_copy, key)) return false;
                continue;
            }
            NeedRemoveResult need_remove_subclass = need_remove_to_subclass(attr_name);
            if (!need_remove_subclass.ok) {
                return false;
            }
            if (need_remove_subclass.base_id) {
                // The class body defines an attribute whose name is private to a
                // parent class and is not declared in this class's own
                // '__private_attrs__'.
                if (!data.use_parents) {
                    // 2.2.0: forbidden unless the parent private attribute is
                    // explicitly inherited via use_parents=True.
                    PyErr_Format(PyExc_TypeError,
                        "cannot define '%s' in class '%s': it is private to a parent class and not declared in this class's '__private_attrs__' (set use_parents=True to inherit parent private attributes)",
                        attr_name.c_str(), data.class_name.c_str());
                    return false;
                }
                // use_parents=True: store the value into this class's own private
                // attribute storage (type_attr_dict[this]) instead of the parent's.
                data.need_remove_itself[attr_name] = need_value;
                if (PyDict_DelItem(data.attrs_copy, key)) return false;
                continue;
            }
            if (value != need_value) {
                if (PyDict_SetItem(data.attrs_copy, key, need_value) < 0) return false;
            }
        }
    }

    // get "private_func" from kwds
    if (kwds && PyDict_Check(kwds)) {
        int has_private_func = PyDict_ContainsString(kwds, "private_func");
        if (has_private_func < 0) {
            return false;
        }
        if (has_private_func) {
            data.private_func = PyDict_GetItemString(kwds, "private_func");
        }
        data.base_kwds = PyObjectStorage::from_new_ref(PyDict_Copy(kwds));
        if (!data.base_kwds) {
            return false;
        }
        if (has_private_func) {
            if (PyDict_DelItemString(data.base_kwds, "private_func") < 0) return false;
        }
        int kwds_has_use_parents = PyDict_ContainsString(kwds, "use_parents");
        if (kwds_has_use_parents < 0) {
            return false;
        }
        if (kwds_has_use_parents) {
            if (PyDict_DelItemString(data.base_kwds, "use_parents") < 0) return false;
        }
    }
    {
        PyObject* original_key;
        Py_ssize_t original_pos = 0;
        PyObject* original_value;
        while (PyDict_Next(attrs, &original_pos, &original_key, &original_value)) {
            std::unordered_set<uintptr_t> set;
            analyse_all_code(original_value, data.code_map, set);
        }
    }
    // v3: inject the special-method structs (__getattribute__ / __getattr__ /
    // __setattr__ / __delattr__ / __del__) into the class namespace so CPython
    // wires tp_getattro / tp_setattro / tp_finalize through them instead of us
    // setting those slots directly. Any user-defined value for one of these
    // names becomes the struct's callback.
    if (!inject_type_slots(data.attrs_copy)) {
        return false;
    }
    data.lock();

    return true;
}

static PyObject*
PrivateAttrType_create(PyTypeObject* type, PrivateAttrCreationData& data) noexcept
{
    PyObjectStorage type_args = PyObjectStorage::from_new_ref(PyTuple_Pack(3, data.name.get(), data.bases.get(), data.attrs_copy.get()));
    if (!type_args) {
        return nullptr;
    }

    PyObject* new_type = PyType_Type.tp_new(type, type_args, data.base_kwds);

    if (!new_type) {
        return nullptr;
    }

    if (!PyObject_IsInstance(new_type, (PyObject*)type)) {
        Py_DECREF(new_type);
        PyErr_SetString(PyExc_TypeError,
                       ("base type creation did not return an instance of '" +
                        std::string(type->tp_name) + "'").c_str());
        return nullptr;
    }

    return new_type;
}

static traverseproc get_need_tp_traverse(PyTypeObject* cls) noexcept;
static inquiry get_need_tp_clear(PyTypeObject* cls) noexcept;

// instance-level traverse/clear for types that use private attributes
static int PrivateAttr_tp_traverse(PyObject* self, visitproc visit, void* arg) noexcept;
static int PrivateAttr_tp_clear(PyObject* self) noexcept;
// shared helper to clear object related data (avoid double clear)
static void clear_object_related(uintptr_t id_self, uintptr_t typ_id) noexcept;
// metaclass-level wrappers (if needed)
static int PrivateAttrType_tp_traverse(PyObject* self, visitproc visit, void* arg) noexcept;
static int PrivateAttrType_tp_clear(PyObject* self) noexcept;

static void
ensure_tp(PyTypeObject* type_instance) noexcept
{
    uintptr_t type_id = (uintptr_t)(type_instance);
    // v3: only tp_traverse / tp_clear are set directly. tp_getattro /
    // tp_setattro / tp_call / tp_finalize are rewired by CPython's
    // update_one_slot when the special-method structs (__getattribute__ /
    // __setattr__ / __delattr__ / __del__) are present in the type dict.
    {
        if (type_instance->tp_traverse != PrivateAttr_tp_traverse) {
            if (!type_instance->tp_traverse && ::AllData::all_type_traverse.find(type_id) == ::AllData::all_type_traverse.end()) {
                ::AllData::all_type_traverse[type_id] = get_need_tp_traverse(type_instance);
            } else {
                ::AllData::all_type_traverse[type_id] = type_instance->tp_traverse;
            }
            type_instance->tp_traverse = PrivateAttr_tp_traverse;
        } else if (::AllData::all_type_traverse.find(type_id) == ::AllData::all_type_traverse.end()) {
            PyTypeObject* base = type_instance->tp_base;
            uintptr_t base_id = (uintptr_t)(base);
            if (::AllData::all_type_traverse.find(base_id) != ::AllData::all_type_traverse.end()) {
                ::AllData::all_type_traverse[type_id] = ::AllData::all_type_traverse[base_id];
            } else if (base && base->tp_traverse && base->tp_traverse != PrivateAttr_tp_traverse) {
                ::AllData::all_type_traverse[type_id] = base->tp_traverse;
            }
        }
    }
    {
        if (type_instance->tp_clear != PrivateAttr_tp_clear) {
            if (!type_instance->tp_clear && ::AllData::all_type_clear.find(type_id) == ::AllData::all_type_clear.end()) {
                ::AllData::all_type_clear[type_id] = get_need_tp_clear(type_instance);
            } else {
                ::AllData::all_type_clear[type_id] = type_instance->tp_clear;
            }
            type_instance->tp_clear = PrivateAttr_tp_clear;
        } else if (::AllData::all_type_clear.find(type_id) == ::AllData::all_type_clear.end()) {
            PyTypeObject* base = type_instance->tp_base;
            uintptr_t base_id = (uintptr_t)(base);
            if (::AllData::all_type_clear.find(base_id) != ::AllData::all_type_clear.end()) {
                ::AllData::all_type_clear[type_id] = ::AllData::all_type_clear[base_id];
            } else if (base && base->tp_clear && base->tp_clear != PrivateAttr_tp_clear) {
                ::AllData::all_type_clear[type_id] = base->tp_clear;
            }
        }
    }
}

static void
ensure_subclass_tp(PyTypeObject* type_instance) noexcept
{
    KeepPythonException _;
    PyObjectStorage subclasses = PyObjectStorage::from_new_ref(PyObject_CallMethod((PyObject*)&PyType_Type, "__subclasses__", "O", (PyObject*)type_instance));
    if (!subclasses) {
        PyObjectStorage unraisable_missing = PyObjectStorage::from_new_ref(PyUnicode_FromString("type.__subclasses__()"));
        PyErr_WriteUnraisable(unraisable_missing);
        return;
    }
    if (!PyList_Check(subclasses)) {
        PyErr_SetString(PyExc_TypeError, "type.__subclasses__() did not return a list");
        PyObjectStorage unraisable_missing = PyObjectStorage::from_new_ref(PyUnicode_FromString("type.__subclasses__()"));
        PyErr_WriteUnraisable(unraisable_missing);
        return;
    }
    Py_ssize_t list_len = PyList_GET_SIZE(subclasses.get());
    for (Py_ssize_t i = 0; i < list_len; i++) {
        PyObject* subclass = PyList_GetItem(subclasses, i);
        if (!PyType_Check(subclass)) continue;
        ensure_tp((PyTypeObject*)subclass);
    }
}

// RAII flag: while active, PrivateAttrType_new captures the pristine
// tp_traverse/tp_clear of the freshly created heap type. type_new assigns
// subtype_traverse/subtype_clear to every heap type unconditionally, so the
// first heap type created through tp_new (PrivateAttrBase) yields the exact
// pointers of CPython's delegating defaults, which the get_*_need_* walkers
// must skip while looking for the real original slot functions.
class PrivateAttrCaptureGuard
{
private:
    static bool& active() noexcept {
        static bool flag = false;
        return flag;
    }

public:
    PrivateAttrCaptureGuard() noexcept {
        active() = true;
    }

    ~PrivateAttrCaptureGuard() noexcept {
        active() = false;
    }

    static bool is_active() noexcept {
        return active();
    }
};

static PyObject*
PrivateAttrType_new(PyTypeObject* type, PyObject* args, PyObject* kwds) noexcept
{
    PrivateAttrCreationData data;
    PyObject* new_type = nullptr;

    if (!PrivateAttrType_preprocess(args, kwds, data, "private_attribute.PrivateAttrType")) {
        return nullptr;
    }

    new_type = PrivateAttrType_create(type, data);
    if (!new_type) {
        return nullptr;
    }

    // Capture CPython's subtype_traverse/subtype_clear pointers: type_new
    // assigns them to every heap type, so right after creation (before
    // ensure_tp wraps the slots) the fresh type's tp_traverse/tp_clear are the
    // exact delegating defaults. The get_*_need_* walkers skip these and keep
    // walking upward to find a real original.
    if (PrivateAttrCaptureGuard::is_active()) {
        PyTypeObject* created = (PyTypeObject*)new_type;
        ::AllData::captured_subtype_traverse = created->tp_traverse;
        ::AllData::captured_subtype_clear = created->tp_clear;
    }

    if (!data.install(new_type)) {
        Py_DECREF(new_type);
        return nullptr;
    }

    return new_type;
}

static PyObject*
PrivateAttrType_call(PyObject* cls, PyObject* args, PyObject* kwds) noexcept
{
    // Finish any pending private-attribute creation before instantiating the
    // class (the creation may have been interrupted before install() ran).
    if (!try_install_pending_creation(cls)) return NULL;
    return PyType_Type.tp_call(cls, args, kwds);
}

static PyObject*
PrivateAttrType_getattr(PyObject* cls, PyObject* name) noexcept
{
    if (!try_install_pending_creation(cls)) return NULL;
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "cls must be a type");
        return NULL;
    }
    uintptr_t typ_id = (uintptr_t)(cls);
    std::string name_str = PyUnicode_AsUTF8(name);
    if (!is_confirmed_public(typ_id, name_str)) {
        PyObjectStorage now_code = PyObjectStorage::from_new_ref(get_now_code());
        TypePrivateAttrResult private_result = type_private_attr(typ_id, name_str);
        if (!private_result.ok) {
            return NULL;
        }
        if (private_result.is_private) {
            if (!now_code) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return NULL;
            }
            AttrClassifyResult classify = attr_classify(typ_id, name_str, (PyCodeObject*)(now_code.get()));
            if (!classify.ok) {
                return NULL;
            }
            if (!classify.allowed) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return NULL;
            }
            if (classify.type_id != 0) {
                return type_getattr(classify.type_id, cls, name_str);
            }
        }
        mark_confirmed_public(typ_id, name_str);
    }
    return PyType_Type.tp_getattro(cls, name);
}

static bool
is_registered_type(PyObject* type) noexcept
{
    uintptr_t typ_id = (uintptr_t)type;
    return (AllData::all_register_type_weak_ref.find(typ_id) != AllData::all_register_type_weak_ref.end());
}

static PyObject* ensure_metaclass_tp(PyObject* /*self*/, PyObject* metaclass) noexcept;
static int ensure_metaclass_tp_head(PyTypeObject* metaclass) noexcept;

static int
new_type_tp_setattro(PyObject* self, PyObject* name, PyObject* value) noexcept
{
    // if the type is immutable raise:
    PyTypeObject* type = (PyTypeObject*)self;
    if (type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) {
        const char* name_cstr = PyUnicode_AsUTF8(name);
        PyErr_Format(PyExc_TypeError, "cannot set '%s' attribute of immutable type '%s'", name_cstr, type->tp_name);
        return -1;
    }
    // v3: when the target is a registered metaclass OR a private-attribute
    // type, and the attribute being set is one of the special methods, write
    // the value directly into the installed slot struct's callback field
    // instead of replacing the struct (which would otherwise break the
    // interception machinery). Metaclasses use the 6-name set (incl.
    // __call__); private-attribute types use the 5-name set.
    if (PyUnicode_Check(name)) {
        const char* name_cstr = PyUnicode_AsUTF8(name);
        if (name_cstr) {
            const char** slot_names = NULL;
            int is_metaclass = 0;
            if (is_registered_type(self)) {
                // if name_cstr is __new__, call update_metaclass_new
                if (strcmp(name_cstr, "__new__") == 0) {
                    PyObject* callback = value;
                    if (value && (PyObject_IsInstance(value, (PyObject*)&PrivateAttrSlotType)
                        || PyObject_IsInstance(value, (PyObject*)&PrivateAttrMetaSlotType)
                        || PyObject_IsInstance(value, (PyObject*)&PrivateAttrMetaNewSlotType))) {
                            callback = ((PrivateAttrSlot*)value) -> callback;
                        }
                    if (!update_metaclass_new((PyTypeObject*)self, value)) {
                        return -1;
                    }
                    return 0;
                }
                slot_names = metaclass_slot_names;
                is_metaclass = 1;
            } else {
                AnalyseStatus analyse_status = need_analyse_type(self);
                if (analyse_status == AnalyseStatus::Error) {
                    return -1;
                }
                if (analyse_status == AnalyseStatus::Yes) {
                    slot_names = type_slot_names;
                }
            }
            // Only rewrite into the callback field for a *user* value. If the
            // value is already one of our slot structs (the initial install
            // done inside type.__new__ / register_metaclass), let the normal
            // type_setattro path install it so update_one_slot wires the slots.
            if (slot_names) {
                for (int i = 0; slot_names[i]; i++) {
                    if (strcmp(name_cstr, slot_names[i]) == 0) {
                        PyObject* callback = value;
                        if (value && (PyObject_IsInstance(value, (PyObject*)&PrivateAttrSlotType)
                            || PyObject_IsInstance(value, (PyObject*)&PrivateAttrMetaSlotType)
                            || PyObject_IsInstance(value, (PyObject*)&PrivateAttrMetaNewSlotType))) {
                                callback = ((PrivateAttrSlot*)value) -> callback;
                            }
                        if (!update_slot_callback((PyTypeObject*)self, name_cstr, callback, is_metaclass)) {
                            return -1;
                        }
                        return 0;
                    }
                }
            }
        }
    }
    return original_type_tp_setattro(self, name, value);
}

static bool has_init_slots = false;

static int
init_all_slots() noexcept
{
    if (has_init_slots || PyType_Type.tp_setattro == new_type_tp_setattro) {
        return 0;
    }
    original_type_tp_setattro = PyType_Type.tp_setattro;
    PyObjectStorage original_setattr = PyObjectStorage::from_new_ref(PyObject_GetAttrString((PyObject*)&PyType_Type, "__setattr__"));
    if (!original_setattr) {return -1;}
    if (original_setattr && PyObject_IsInstance(original_setattr, (PyObject*)&PyWrapperDescr_Type)) {
        ((PyWrapperDescrObject*)(original_setattr.get()))->d_wrapped = (void*)new_type_tp_setattro;
    }
    PyObjectStorage original_delattr = PyObjectStorage::from_new_ref(PyObject_GetAttrString((PyObject*)&PyType_Type, "__delattr__"));
    if (!original_delattr) {return -1;}
    if (original_delattr && PyObject_IsInstance(original_delattr, (PyObject*)&PyWrapperDescr_Type)) {
        ((PyWrapperDescrObject*)(original_delattr.get()))->d_wrapped = (void*)new_type_tp_setattro;
    }
    PyType_Type.tp_setattro = new_type_tp_setattro;
    PyObjectStorage all_metaclasses = PyObjectStorage::from_new_ref(PyObject_CallMethod((PyObject*)&PyType_Type, "__subclasses__", "O", (PyObject*)&PyType_Type));
    if (!all_metaclasses) {return -1;}
    if (!PyList_Check(all_metaclasses)) {
        PyErr_SetString(PyExc_TypeError, "subclasses of type is not list");
        return -1;
    }
    Py_ssize_t list_len = PyList_GET_SIZE(all_metaclasses.get());
    for (Py_ssize_t i = 0; i < list_len; i++) {
        PyObject* metaclass = PyList_GET_ITEM(all_metaclasses.get(), i);
        if (!PyType_Check(metaclass)) {continue;}
        if (((PyTypeObject*)metaclass)->tp_setattro == original_type_tp_setattro) {
            ((PyTypeObject*)metaclass)->tp_setattro = new_type_tp_setattro;
        }
    }
    has_init_slots = true;
    return 0;
}

static traverseproc
get_need_tp_traverse(PyTypeObject* cls) noexcept
{
    PyTypeObject* base = cls->tp_base;
    while (base) {
        if (base->tp_traverse != PrivateAttr_tp_traverse
            && base->tp_traverse != ::AllData::captured_subtype_traverse) {
            return base->tp_traverse;
        } else if (::AllData::all_type_traverse.find((uintptr_t)base) != ::AllData::all_type_traverse.end()) {
            traverseproc saved = ::AllData::all_type_traverse[(uintptr_t)base];
            if (saved && saved != ::AllData::captured_subtype_traverse) {
                return saved;
            }
            base = base->tp_base;
        } else {
            base = base->tp_base;
        }
    }
    return NULL;
}

static inquiry
get_need_tp_clear(PyTypeObject* cls) noexcept
{
    PyTypeObject* base = cls->tp_base;
    while (base) {
        if (base->tp_clear != PrivateAttr_tp_clear
            && base->tp_clear != ::AllData::captured_subtype_clear) {
            return base->tp_clear;
        } else if (::AllData::all_type_clear.find((uintptr_t)base) != ::AllData::all_type_clear.end()) {
            inquiry saved = ::AllData::all_type_clear[(uintptr_t)base];
            if (saved && saved != ::AllData::captured_subtype_clear) {
                return saved;
            }
            base = base->tp_base;
        } else {
            base = base->tp_base;
        }
    }
    return NULL;
}

static traverseproc
get_type_need_tp_traverse(PyTypeObject* cls) noexcept
{
    // cls is a type object (an instance of a metaclass). CPython only invokes
    // a real traverse on a tracked type object directly; otherwise it delegates
    // through subtype_traverse. So walk the metaclass base chain and skip every
    // delegating default (subtype_traverse, PrivateAttr_tp_traverse, our own
    // wrapper) AND NULL slots, returning the nearest real implementation.
    //
    // For a plain metaclass chain the walk is
    //   cls -> ... -> PrivateAttrBase -> object -> (tp_base == NULL)
    // and object->tp_traverse is NULL, so the loop keeps walking to the end and
    // falls through to the PyType_Type.tp_traverse fallback below: for a type
    // object the ONLY correct real traverse is CPython's own type_traverse,
    // which is what makes the class's hard self-cycle (tp_dict / tp_mro /
    // tp_bases) visible to the cyclic GC. Without that fallback the class
    // becomes uncollectable. For a REGISTERED metaclass chain the saved-map
    // branch may find a real implementation before the fallback is reached.
    PyTypeObject* base = cls->tp_base;
    while (base) {
        traverseproc slot = base->tp_traverse;
        if (slot
            && slot != PrivateAttrType_tp_traverse
            && slot != ::AllData::captured_subtype_traverse
            && slot != PrivateAttr_tp_traverse) {
            return slot;
        }
        auto it = ::AllData::all_type_traverse.find((uintptr_t)base);
        if (it != ::AllData::all_type_traverse.end()) {
            traverseproc saved = it->second;
            if (saved
                && saved != ::AllData::captured_subtype_traverse
                && saved != PrivateAttr_tp_traverse
                && saved != PrivateAttrType_tp_traverse) {
                return saved;
            }
        }
        base = base->tp_base;
    }
    // Fallback to CPython's real type_traverse (PyType_Type.tp_traverse). A
    // NULL slot is never a real implementation, so exhausting the chain here
    // means no custom traverse exists and the only correct original for a type
    // object is type_traverse itself.
    return PyType_Type.tp_traverse;
}

static inquiry
get_type_need_tp_clear(PyTypeObject* cls) noexcept
{
    // Same as get_type_need_tp_traverse: skip every delegating default
    // (subtype_clear, PrivateAttr_tp_clear, our own wrapper) AND NULL slots,
    // then fall back to CPython's real type_clear (PyType_Type.tp_clear) so a
    // plain metaclass chain still breaks the class's hard self-cycle.
    PyTypeObject* base = cls->tp_base;
    while (base) {
        inquiry slot = base->tp_clear;
        if (slot
            && slot != PrivateAttrType_tp_clear
            && slot != ::AllData::captured_subtype_clear
            && slot != PrivateAttr_tp_clear) {
            return slot;
        }
        auto it = ::AllData::all_type_clear.find((uintptr_t)base);
        if (it != ::AllData::all_type_clear.end()) {
            inquiry saved = it->second;
            if (saved
                && saved != ::AllData::captured_subtype_clear
                && saved != PrivateAttr_tp_clear
                && saved != PrivateAttrType_tp_clear) {
                return saved;
            }
        }
        base = base->tp_base;
    }
    return PyType_Type.tp_clear;
}

static int
PrivateAttrType_setattr(PyObject* cls, PyObject* name, PyObject* value) noexcept
{
    if (!try_install_pending_creation(cls)) return -1;
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "cls must be a type");
        return -1;
    }
    // check Py_TPFLAGS_IMMUTABLETYPE
    uintptr_t typ_id = (uintptr_t)(cls);
    std::string name_str = PyUnicode_AsUTF8(name);
    if (((PyTypeObject*)cls)->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) {
        PyErr_Format(PyExc_TypeError, "cannot set '%s' attribute of immutable type '%s'", name_str.c_str(), ((PyTypeObject*)cls)->tp_name);
        return -1;
    }
    if (!is_confirmed_public(typ_id, name_str)) {
        PyObjectStorage now_code = PyObjectStorage::from_new_ref(get_now_code());
        TypePrivateAttrResult private_result = type_private_attr(typ_id, name_str);
        if (!private_result.ok) {
            return -1;
        }
        if (private_result.is_private) {
            if (!now_code) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return -1;
            }
            AttrClassifyResult classify = attr_classify(typ_id, name_str, (PyCodeObject*)(now_code.get()));
            if (!classify.ok) {
                return -1;
            }
            if (!classify.allowed) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return -1;
            }
            if (classify.type_id != 0) {
                return type_setattr(classify.type_id, cls, name_str, value);
            }
            // owner is 0: not private for this access context -> normal attribute
        }
        mark_confirmed_public(typ_id, name_str);
    }
    return new_type_tp_setattro(cls, name, value);
}

static void
PrivateAttrType_finalize(PyObject* cls) noexcept
{
    uintptr_t typ_id = (uintptr_t) cls;
    if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()) {
        ::AllData::all_type_attr_set.erase(typ_id);
    }
    if (::AllData::type_allowed_code_map.find(typ_id) != ::AllData::type_allowed_code_map.end()) {
        ::AllData::type_allowed_code_map.erase(typ_id);
    }
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        ::AllData::type_need_call.erase(typ_id);
    }
    if (::AllData::type_attr_dict.find(typ_id) != ::AllData::type_attr_dict.end()) {
        ::AllData::type_attr_dict.erase(typ_id);
    }
    if (::AllData::all_type_subclass_attr.find(typ_id) != ::AllData::all_type_subclass_attr.end()) {
        ::AllData::all_type_subclass_attr.erase(typ_id);
    }
    if (::AllData::all_type_subclass_mutex.find(typ_id) != ::AllData::all_type_subclass_mutex.end()) {
        ::AllData::all_type_subclass_mutex.erase(typ_id);
    }
    std::vector<uintptr_t> parent_ids;
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()) {
        parent_ids = ::AllData::all_type_parent_id[typ_id];
        ::AllData::all_type_parent_id.erase(typ_id);
    }
    for (auto& parent_id : parent_ids) {
        if (::AllData::all_type_subclass_attr.find(parent_id) != ::AllData::all_type_subclass_attr.end()) {
            if (::AllData::all_type_subclass_attr[parent_id].find(typ_id) != ::AllData::all_type_subclass_attr[parent_id].end()) {
                ::AllData::all_type_subclass_attr[parent_id].erase(typ_id);
            }
        }
        if (::AllData::all_type_subclass_mutex.find(parent_id) != ::AllData::all_type_subclass_mutex.end()) {
            if (::AllData::all_type_subclass_mutex[parent_id].find(typ_id) != ::AllData::all_type_subclass_mutex[parent_id].end()) {
                ::AllData::all_type_subclass_mutex[parent_id].erase(typ_id);
            }
        }
    }
    ::AllData::all_type_mutex.erase(typ_id);
    // Clear the per-type buckets of the object-attr tables (created in
    // PrivateAttrType_postprocess / PrivateAttr_object_init_private_dict);
    // otherwise every class that ever got instances leaves an empty bucket
    // behind and all_object_attr / all_object_mutex grow forever.
    if (::AllData::all_object_attr.find(typ_id) != ::AllData::all_object_attr.end()) {
        ::AllData::all_object_attr.erase(typ_id);
    }
    if (::AllData::all_object_mutex.find(typ_id) != ::AllData::all_object_mutex.end()) {
        ::AllData::all_object_mutex.erase(typ_id);
    }
    clear_public_obj(typ_id);
    clear_obj(typ_id);
}

static void
PrivateAttrType_del(PyObject* cls) noexcept
{
    PrivateAttrType_finalize(cls);
    PyType_Type.tp_dealloc(cls);
}

static const char* PrivateAttrBase_doc = "The class to help creating private attribute. It does not have any special behavior.";

// PrivateAttrBase
static PyObject*
create_private_attr_base_simple(void) noexcept
{
    PyObjectStorage name = PyObjectStorage::from_new_ref(PyUnicode_InternFromString("PrivateAttrBase"));
    if (!name) return NULL;

    PyObjectStorage bases = PyObjectStorage::from_new_ref(PyTuple_New(0));
    if (!bases) return NULL;

    PyObjectStorage dict = PyObjectStorage::from_new_ref(PyDict_New());
    if (!dict) return NULL;

    PyObjectStorage private_attrs = PyObjectStorage::from_new_ref(PyTuple_New(0));
    if (!private_attrs) return NULL;

    PyObjectStorage doc = PyObjectStorage::from_new_ref(PyUnicode_InternFromString(PrivateAttrBase_doc));
    if (!doc) return NULL;

    PyObjectStorage module_name = PyObjectStorage::from_new_ref(PyUnicode_InternFromString("private_attribute"));
    if (!module_name) return NULL;

    if (PyDict_SetItemString(dict, "__private_attrs__", private_attrs) < 0) return NULL;
    if (PyDict_SetItemString(dict, "__slots__", private_attrs) < 0) return NULL;
    if (PyDict_SetItemString(dict, "__doc__", doc) < 0) return NULL;
    if (PyDict_SetItemString(dict, "__module__", module_name) < 0) return NULL;

    PyObjectStorage args = PyObjectStorage::from_new_ref(PyTuple_Pack(3, name.get(), bases.get(), dict.get()));
    if (!args) return NULL;

    PyObject* base_type = NULL;
    {
        // RAII: the first heap type created through tp_new (PrivateAttrBase)
        // carries CPython's pristine subtype_traverse/subtype_clear in its
        // tp_traverse/tp_clear; PrivateAttrType_new captures those pointers
        // while this guard is active.
        PrivateAttrCaptureGuard capture_guard;
        base_type = PrivateAttrType_new((PyTypeObject*)&PrivateAttrType, args.get(), NULL);
    }

    if (!base_type) return NULL;

    ((PyTypeObject*)base_type)->tp_flags |= Py_TPFLAGS_IMMUTABLETYPE;
    return base_type;
}

typedef struct {
    PyObject_HEAD
    PrivateAttrCreationData* tmp;
} PrivateTempObject;

static PyObject*
PrivateTempObject_name(PyObject* self, void* /*closure*/) noexcept
{
    PyObject* name = ((PrivateTempObject*)self)->tmp->name;
    if (!name) {
        PyErr_SetString(PyExc_RuntimeError, "object not init or have been used");
        return nullptr;
    }
    Py_INCREF(name);
    return name;
}

static PyObject*
PrivateTempObject_base(PyObject* self, void* /*closure*/) noexcept
{
    PyObject* base = ((PrivateTempObject*)self)->tmp->bases;
    if (!base) {
        PyErr_SetString(PyExc_RuntimeError, "object not init or have been used");
        return nullptr;
    }
    Py_INCREF(base);
    return base;
}

static PyObject*
PrivateTempObject_attrs(PyObject* self, void* /*closure*/) noexcept
{
    PyObject* attrs = ((PrivateTempObject*)self)->tmp->attrs_copy;
    if (!attrs) {
        PyErr_SetString(PyExc_RuntimeError, "object not init or have been used");
        return nullptr;
    }
    Py_INCREF(attrs);
    return attrs;
}

static PyObject*
PrivateTempObject_kwds(PyObject* self, void* /*closure*/) noexcept
{
    PyObject* kwds = ((PrivateTempObject*)self)->tmp->base_kwds;
    if (!kwds) {
        return PyDict_New();
    }
    Py_INCREF(kwds);
    return kwds;
}

static PyGetSetDef PrivateTempObject_getsets[] = {
    {"name", (getter)PrivateTempObject_name, NULL, "The name for submetaclass.__new__ argument 1", NULL},
    {"bases", (getter)PrivateTempObject_base, NULL, "The base classes for submetaclass.__new__ argument 2", NULL},
    {"attrs", (getter)PrivateTempObject_attrs, NULL, "The attributes for submetaclass.__new__ argument 3", NULL},
    {"kwds", (getter)PrivateTempObject_kwds, NULL, "The keyword arguments for submetaclass.__new__", NULL},
    {NULL}
};

static void
PrivateTempObject_dealloc(PyObject* self) noexcept
{
    ((PrivateTempObject*)self)->tmp->clear();
    delete ((PrivateTempObject*)self)->tmp;
    Py_TYPE(self)->tp_free(self);
}

static PyObject*
PrivateTempObject_postprocess(PyObject* self, PyObject* type) noexcept
{
    if (!PyType_Check(type)) {
        PyErr_SetString(PyExc_TypeError, "type must be a type");
        return NULL;
    }
    if ((((PrivateTempObject*)self)->tmp->cleared)) {
        PyErr_SetString(PyExc_RuntimeError, "temp not init or have been used");
        return NULL;
    }
    if (!(((PrivateTempObject*)self)->tmp->install(type))) {
        return NULL;
    }
    ((PrivateTempObject*)self)->tmp->clear();
    Py_RETURN_NONE;
}

static PyObject*
PrivateTempObject_enter(PyObject* self, PyObject* /*args*/) noexcept
{
    Py_INCREF(self);
    return self;
}


static PyObject*
PrivateTempObject_exit(PyObject* self, PyObject* /*args*/) noexcept
{
    PrivateTempObject* obj = (PrivateTempObject*)self;
    if (!obj->tmp->cleared) {
        obj->tmp->clear();
    }
    Py_RETURN_NONE;
}

static PyMethodDef PrivateTempObject_methods[] = {
    {"postprocess", (PyCFunction)PrivateTempObject_postprocess, METH_O, "postprocess the temp object"},
    {"__enter__", (PyCFunction)PrivateTempObject_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)PrivateTempObject_exit, METH_VARARGS, NULL},
    {NULL}
};

static int
PrivateTempObject_traverse(PyObject* self, visitproc visit, void* arg) noexcept
{
    PrivateTempObject* tmp = (PrivateTempObject*)self;
    if (tmp->tmp) {
        PrivateAttrCreationData* data = tmp->tmp;
        if (!data->cleared) {
            Py_VISIT(data->attrs_copy);
            Py_VISIT(data->base_kwds);
        }
    }
    return 0;
}

static int
PrivateTempObject_clear(PyObject* self) noexcept
{
    PrivateTempObject* tmp = (PrivateTempObject*)self;
    if (tmp->tmp && !tmp->tmp->cleared) {
        tmp->tmp->clear();
    }
    return 0;
}

static PyTypeObject PrivateTempType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute._PrivateTemp",    // tp_name
    sizeof(PrivateTempObject), // tp_basicsize
    0, //tp_itemsize
    PrivateTempObject_dealloc, //tp_dealloc
    0, //tp_print
    0, //tp_getattr
    0, //tp_setattr
    0, //tp_compare
    0, //tp_repr
    0, //tp_as_number
    0, //tp_as_sequence
    0, //tp_as_mapping
    0, //tp_hash
    0, //tp_call
    0, //tp_str
    0, //tp_getattro
    0, //tp_setattro
    0, //tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, //tp_flags
    0, //tp_doc
    PrivateTempObject_traverse, //tp_traverse
    PrivateTempObject_clear, //tp_clear
    0, //tp_richcompare
    0, //tp_weaklistoffset
    0, //tp_iter
    0, //tp_iternext
    PrivateTempObject_methods, //tp_methods
    0, //tp_members
    PrivateTempObject_getsets //tp_getset
};

static PyObject*
prepare_for_PrivateAttr(PyObject* /*self*/, PyObject* args, PyObject* kwargs) noexcept
{
    PrivateAttrCreationData* tmp_data = new(std::nothrow) PrivateAttrCreationData();
    if (!tmp_data) {
        return PyErr_NoMemory();
    }
    if (!PrivateAttrType_preprocess(args, kwargs, *tmp_data, "private_attribute.prepare")) {
        tmp_data->clear();
        delete tmp_data;
        return NULL;
    }
    PrivateTempObject* tmp = PyObject_GC_New(PrivateTempObject, &PrivateTempType);
    if (!tmp) {
        tmp_data->clear();
        delete tmp_data;
        return NULL;
    }
    tmp->tmp = tmp_data;
    PyObject_GC_Track(tmp);
    return (PyObject*)tmp;
}

static PyObject*
postprocess_for_PrivateAttr(PyObject* /*self*/, PyObject* args) noexcept
{
    PyObject* type;
    PyObject* tmp;
    if (!PyArg_ParseTuple(args, "OO:private_attribute.postprocess", &type, &tmp)) {
        return NULL;
    }
    if (!PyObject_IsInstance(tmp, (PyObject*)&PrivateTempType)) {
        PyErr_SetString(PyExc_TypeError, "tmp must be a PrivateTempObject");
        return NULL;
    }
    return PrivateTempObject_postprocess(tmp, type);
}

#define METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(name) do {\
    if (::AllData::all_type_##name.find(id) != ::AllData::all_type_##name.end()) {\
        ::AllData::all_type_##name.erase(id);\
    }\
} while (0)

static PyObject*
metaclass_weakref_callback(PyObject* /*self*/, PyObject* weakref) noexcept
{
    uintptr_t weakref_id = (uintptr_t)weakref;

    std::unique_lock lock(::AllData::all_register_new_metaclass_mutex);
    if (::AllData::weakref_type.find(weakref_id) == ::AllData::weakref_type.end()) Py_RETURN_NONE;
    uintptr_t id = ::AllData::weakref_type[weakref_id];
    ::AllData::weakref_type.erase(weakref_id);

    auto it = ::AllData::all_register_type_weak_ref.find(id);
    if (it != ::AllData::all_register_type_weak_ref.end()) {
        // The PyObjectStorage destructor releases the stored reference on
        // erase; do not DECREF here (double-DECREF).
        ::AllData::all_register_type_weak_ref.erase(it);
    }
    METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(traverse);
    METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(clear);

    Py_RETURN_NONE;
}

static PyMethodDef weakref_callback_def = {
    "metaclass_weakref_callback",
    (PyCFunction)metaclass_weakref_callback,
    METH_O,
    NULL
};

static bool
is_class_of_registed_type(PyObject* metaclass) noexcept
{
    std::shared_lock lock(::AllData::all_register_new_metaclass_mutex);
    for (auto& [id, instanceref]: ::AllData::all_register_type_weak_ref){
        if (!PyWeakref_CheckRef(instanceref)) {
            continue;
        }
#if PY_VERSION_HEX < 0x030D0000
        PyObjectStorage instance = PyWeakref_GET_OBJECT((PyObject*)instanceref);
        if (instance.get() == metaclass) continue;
        if (PyObject_IsInstance(instance.get(), metaclass)) {
            return true;
        }
#else
        PyObjectStorage instance;
        if (PyWeakref_GetRef(instanceref, instance.getpointer()) == 1) {
            if (instance == metaclass) {
                continue;
            }
            if (PyObject_IsInstance(instance, metaclass)) {
                return true;
            }
        }
#endif
    }
    return false;
}

static PyObject*
register_metaclass_head(PyObject* metaclass) noexcept
{
    if (!PyType_Check(metaclass)) {
        PyErr_SetString(PyExc_TypeError, "metaclass must be a type");
        return NULL;
    }
    if (!PyObject_IsSubclass(metaclass, (PyObject*)&PyType_Type)) {
        PyErr_SetString(PyExc_TypeError, "metaclass must be a metatype");
        return NULL;
    }
    if (metaclass == (PyObject*)&PyType_Type) {
        PyErr_SetString(PyExc_ValueError, "cannot register type itself");
        return NULL;
    }
    // v3: registered types and created types must be heap types. Static types
    // cannot be rewired by update_one_slot (their slots are baked into the
    // static type object), so refuse to register them.
    if (!(((PyTypeObject*)metaclass)->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_SetString(PyExc_TypeError, "cannot register a static (non-heap) type");
        return NULL;
    }
    AnalyseStatus analyse_status = need_analyse_type(metaclass);
    if (analyse_status == AnalyseStatus::Error) {
        return NULL;
    }
    if (analyse_status == AnalyseStatus::Yes) {
        PyErr_SetString(PyExc_ValueError, "cannot register type that is the instance of 'PrivateAttrType' or registed type");
        return NULL;
    }
    if (is_class_of_registed_type(metaclass)) {
        PyErr_SetString(PyExc_ValueError, "cannot register type that is the class of registed type");
        return NULL;
    }
    uintptr_t id = (uintptr_t)metaclass;

    std::unique_lock lock(::AllData::all_register_new_metaclass_mutex);
    if (::AllData::all_register_type_weak_ref.find(id) != ::AllData::all_register_type_weak_ref.end()) {
        Py_RETURN_NONE;
    }

    {
        PyObjectStorage callback = PyObjectStorage::from_new_ref(PyCFunction_New(&weakref_callback_def, NULL));
        if (!callback) {
            return NULL;
        }
        PyObjectStorage ref = PyObjectStorage::from_new_ref(PyWeakref_NewRef(metaclass, callback.get()));
        if (!ref) {
            return NULL;
        }
        // PyWeakref_NewRef returns a new reference; transfer it into the storage
        // without an extra INCREF (which the plain assignment operator would add).
        ::AllData::all_register_type_weak_ref[id] = ref;
        ::AllData::weakref_type[(uintptr_t)(ref.get())] = id;
    }

    // A newly registered metaclass can turn previously-created classes into
    // private-attribute types (any type whose metaclass is, or derives from,
    // this metaclass now needs analysis). Drop the cached need_analyse_type
    // results so they are recomputed against the updated metaclass set.
    {
        std::unique_lock cache_lock(::AllData::is_private_mutex);
        ::AllData::is_private_cache.clear();
        ::AllData::is_private_cache_weakref_dict.clear();
    }
    PyTypeObject* mt = (PyTypeObject*)metaclass;
    // v3: explicitly install the metaclass special-method structs
    // (__getattribute__ / __getattr__ / __setattr__ / __delattr__ / __del__ /
    // __call__) so CPython's update_one_slot rewires tp_getattro /
    // tp_setattro / tp_call. Only tp_traverse / tp_clear are set directly
    // (via ensure_metaclass_tp_head), kept decoupled from slot installation.
    if (!install_metaclass_slots(mt)) {
        return NULL;
    }
    if (ensure_metaclass_tp_head(mt) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject*
register_metaclass(PyObject* /*self*/, PyObject* metaclass) noexcept
{
    if (!register_metaclass_head(metaclass)) return NULL;
    PyObjectStorage subclasses = PyObjectStorage::from_new_ref(PyObject_CallMethod((PyObject*)&PyType_Type, "__subclasses__", "O", metaclass));
    if (!subclasses) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    if (!PyList_Check(subclasses.get())) {
        Py_RETURN_NONE;
    }
    Py_ssize_t list_len = PyList_GET_SIZE(subclasses.get());
    for (Py_ssize_t i = 0; i < list_len; i++) {
        PyObject* subclass = PyList_GET_ITEM(subclasses.get(), i);
        if (!register_metaclass(0, subclass)) {
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject*
ensure_type_tp(PyObject* /*self*/, PyObject* type) noexcept
{
    if (!PyType_Check(type)) {
        PyErr_SetString(PyExc_TypeError, "type must be a type");
        return NULL;
    }
    AnalyseStatus analyse_status = need_analyse_type(type);
    if (analyse_status == AnalyseStatus::Error) {
        return NULL;
    }
    if (analyse_status == AnalyseStatus::No) {
        Py_RETURN_NONE;
    }
    ensure_tp((PyTypeObject*)type);
    ensure_subclass_tp((PyTypeObject*)type);
    Py_RETURN_NONE;
}

static int
ensure_metaclass_tp_head(PyTypeObject* metaclass) noexcept
{
    uintptr_t id = (uintptr_t)metaclass;
    if (::AllData::all_register_type_weak_ref.find(id) != ::AllData::all_register_type_weak_ref.end()) {
        PyTypeObject* mt = (PyTypeObject*)metaclass;
        // v3: only tp_traverse / tp_clear are set directly. The special-method
        // slot structs are installed explicitly (install_metaclass_slots) at
        // registration time, NOT here — keeping the two responsibilities
        // decoupled.
        if (mt->tp_traverse != PrivateAttrType_tp_traverse) {
            if (::AllData::all_type_traverse.find(id) == ::AllData::all_type_traverse.end()) {
                ::AllData::all_type_traverse[id] = get_type_need_tp_traverse(mt);
            }
            mt->tp_traverse = PrivateAttrType_tp_traverse;
        }
        if (mt->tp_clear != PrivateAttrType_tp_clear) {
            if (::AllData::all_type_clear.find(id) == ::AllData::all_type_clear.end()) {
                ::AllData::all_type_clear[id] = get_type_need_tp_clear(mt);
            }
            mt->tp_clear = PrivateAttrType_tp_clear;
        }
    }
    return 0;
}

static PyObject*
ensure_metaclass_tp(PyObject* /*self*/, PyObject* metaclass) noexcept
{
    // v3: only tp_traverse / tp_clear are (re)applied here. Slot installation
    // is handled explicitly by register_metaclass_head / install_metaclass_slots.
    if (ensure_metaclass_tp_head((PyTypeObject*)metaclass) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject*
PrivateModule_get_PrivateWrapProxy(PyObject* /*self*/, void* /*closure*/) noexcept
{
    PyObject* PythonPrivateWrapProxy = (PyObject*)&PrivateWrapProxyType;
    Py_INCREF(PythonPrivateWrapProxy);
    return PythonPrivateWrapProxy;
}

static PyObject*
PrivateModule_get_PrivateSuper(PyObject* /*self*/, void* /*closure*/) noexcept
{
    PyObject* o = (PyObject*)&PrivateSuperType;
    Py_INCREF(o);
    return o;
}

static PyObject*
PrivateModule_get_PrivateClassSuper(PyObject* /*self*/, void* /*closure*/) noexcept
{
    PyObject* o = (PyObject*)&PrivateClassSuperType;
    Py_INCREF(o);
    return o;
}

// type PrivateAttrType
static PyObject*
PrivateModule_get_PrivateAttrType(PyObject* /*self*/, void* /*closure*/) noexcept
{
    PyObject* PythonPrivateAttrType = (PyObject*)&PrivateAttrType;
    Py_INCREF(PythonPrivateAttrType);
    return PythonPrivateAttrType;
}

static PyObject*
PrivateModule_get_PrivateAttrBase(PyObject* /*self*/, void* /*closure*/) noexcept
{
    static std::mutex base_mutex;
    std::lock_guard<std::mutex> lock(base_mutex);
    auto it = AllData::store_module_self.find(AllData::kPrivateAttrBaseKey);
    if (it == AllData::store_module_self.end()) {
        PyObject* base = create_private_attr_base_simple();
        if (!base) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_RuntimeError, "failed to create PrivateAttrBase");
            }
            return NULL;
        }
        AllData::store_module_self[AllData::kPrivateAttrBaseKey] = base;
        return base;
    }
    PyObject* PrivateAttrBase = it->second.get();
    Py_INCREF(PrivateAttrBase);
    return PrivateAttrBase;
}

static PyObject*
PrivateModule_dir(PyObject* self, PyObject* /*args*/) noexcept
{
    PyObjectStorage parent_dir = PyObjectStorage::from_new_ref(PyObject_CallMethod((PyObject*)&PyModule_Type, "__dir__", "O", self));
    if (!parent_dir) return NULL;
    PyObjectStorage attr_list = PyObjectStorage::from_new_ref(Py_BuildValue("[sssssssssssss]",
        "PrivateWrapProxy",
        "PrivateAttrType",
        "PrivateAttrBase",
        "PrivateSuper",
        "PrivateClassSuper",
        "prepare",
        "postprocess",
        "register_metaclass",
        "ensure_type",
        "ensure_metaclass",
        "create_new",
        "create_meta_slot",
        "create_slot"
    ));
    if (!attr_list) {
        return NULL;
    }
    PyObject* result = PySequence_Concat(parent_dir, attr_list);
    return result;
}

#define PrivateModule_GETATTRO_CASE(name) \
    do {\
        if (strcmp(name_cstr, #name) == 0) {\
            PyObject* attr = PrivateModule_get_##name(NULL, NULL);\
            return attr;\
        }\
    } while (0)

static int
PrivateModule_setattro(PyObject* self, PyObject* name, PyObject* value) noexcept
{
    // if name is "__class__" it do nothing and return success
    if (PyUnicode_Check(name)) {
        const char* name_cstr = PyUnicode_AsUTF8(name);
        if (name_cstr && strcmp(name_cstr, "__class__") == 0) {
            return 0;
        }
        static const char* unsetable_attrs[] = {
            "PrivateWrapProxy",
            "PrivateAttrType",
            "PrivateAttrBase",
            "PrivateSuper",
            "PrivateClassSuper",
            "prepare",
            "postprocess",
            "register_metaclass",
            "ensure_type",
            "ensure_metaclass",
            "create_new",
            NULL
        };
        for (size_t i = 0; unsetable_attrs[i]; ++i) {
            if (strcmp(name_cstr, unsetable_attrs[i]) == 0) {
                PyErr_Format(PyExc_AttributeError, "attribute '%s' of 'private_attribute.private_attribute_module' objects is not writable", name_cstr);
                return -1;
            }
        }
    }
    return PyModule_Type.tp_setattro(self, name, value);
}

static const char* prepare_and_postprocess_doc = R"(function for custom metaclass to create private attributes class.

def prepare(name: str, bases: tuple, attrs: dict, **kwds) -> tempobject:
    the function to prepare for creating private attributes class. It will return a temporary object which has the same information as the arguments.

def postprocess(type: type, tmp: tempobject) -> None:
    the function to postprocess for creating private attributes class. The custom metaclass can call this

def register_metaclass(metaclass: type) -> None:
    the function to register custom metaclass. The custom metaclass must call this function to register itself before creating any private attributes class,
    otherwise the private attributes class created by this custom metaclass will not work.

All usage of this module should be like:
```
from abc import ABCMeta
import private_attribute

class PrivateAbcMeta(ABCMeta):
    def __new__(cls, *args, **kwargs):
        with private_attribute.prepare(*args, **kwargs) as temp:
            typ = super().__new__(cls, temp.name, temp.bases, temp.attrs, **temp.kwds)
            private_attribute.postprocess(typ, temp) # or temp.postprocess(typ)
            return typ

private_attribute.register_metaclass(PrivateAbcMeta)
```

**NOTE**:
    Deprecated. See "create_new".
)";

static const char* ensure_type_doc = R"(function for custom metaclass to ensure the type is a private attributes class.
def ensure_type(type: type) -> None:
    the function to re-inject the special-method slot descriptors
    (__getattribute__/__getattr__/__setattr__/__delattr__/__del__) and the
    tp_traverse/tp_clear wrappers after CPython has reset them.
)";

static const char* ensure_metaclass_doc = R"(function for custom metaclass to ensure the metaclass is working.
def ensure_metaclass(metaclass: type) -> None:
    the function to re-inject the metaclass slot descriptors
    (__getattribute__/__getattr__/__setattr__/__delattr__/__del__/__call__)
    and the tp_traverse/tp_clear wrappers.
)";

static const char* create_new_doc = R"(function to register the type with new automatically:

```
from abc import ABCMeta
import private_attribute

class PrivateAbcMeta(ABCMeta):
    __new__ = private_attribute.create_new()
```

also as a decorator:

```
from abc import ABCMeta
import private_attribute

class PrivateAbcMeta(ABCMeta):
    @private_attribute.create_new
    def __new__(cls, *args, **kwargs):
        ...
```
)";

static const char* create_meta_slot_doc = R"(function to create the final slot for metaclass:

```
from abc import ABCMeta
import private_attribute

class PrivateAbcMeta(ABCMeta):
    __new__ = private_attribute.create_new()

    __setattr__ = private_attribute.create_meta_slot("__setattr__")()
```

As a decorator:

```
from abc import ABCMeta
import private_attribute

class PrivateAbcMeta(ABCMeta):
    __new__ = private_attribute.create_new()

    @private_attribute.create_meta_slot("__setattr__")
    def __setattr__(self, name, value):
        ...
```
)";

static const char* create_slot_doc = R"(function to create the final slot for private class:

```
import private_attribute

class MyClass(private_attribute.PrivateAttrBase):
    __setattr__ = private_attribute.create_slot("__setattr__")()

```
As a decorator:

```
import private_attribute

class MyClass(private_attribute.PrivateAttrBase):
    @private_attribute.create_slot("__setattr__")
    def __setattr__(self, name, value):
        ...

```
)";

static PyMethodDef PrivateModule_methods[] = {
    {"prepare", (PyCFunction)prepare_for_PrivateAttr, METH_VARARGS | METH_KEYWORDS | METH_STATIC, prepare_and_postprocess_doc},
    {"postprocess", (PyCFunction)postprocess_for_PrivateAttr, METH_VARARGS | METH_STATIC, prepare_and_postprocess_doc},
    {"register_metaclass", (PyCFunction)register_metaclass, METH_O | METH_STATIC, prepare_and_postprocess_doc},
    {"ensure_type", (PyCFunction)ensure_type_tp, METH_O | METH_STATIC, ensure_type_doc},
    {"ensure_metaclass", (PyCFunction)ensure_metaclass_tp, METH_O | METH_STATIC, ensure_metaclass_doc},
    {"create_new", (PyCFunction)register_type_create_new, METH_VARARGS, create_new_doc},
    {"create_meta_slot", (PyCFunction)register_type_create_slot, METH_O, create_meta_slot_doc},
    {"create_slot", (PyCFunction)private_type_create_slot, METH_O, create_slot_doc},
    {NULL}  // Sentinel
};

static PyObject*
PrivateModule_get_prepare(PyObject* /*self*/, void* /*closure*/) noexcept
{
    // warning that it is deprecated. See "create_new"
    if (PyErr_WarnEx(NULL, "private_attribute.prepare is deprecated. See 'private_attribute.create_new'.", 2) < 0) {
        return NULL;
    }
    return PyCFunction_NewEx(&PrivateModule_methods[0], NULL, NULL);
}

static PyObject*
PrivateModule_get_postprocess(PyObject* /*self*/, void* /*closure*/) noexcept
{
    // warning that it is deprecated. See "create_new"
    if (PyErr_WarnEx(NULL, "private_attribute.postprocess is deprecated. See 'private_attribute.create_new'.", 2) < 0) {
        return NULL;
    }
    return PyCFunction_NewEx(&PrivateModule_methods[1], NULL, NULL);
}

static PyObject*
PrivateModule_get_register_metaclass(PyObject* /*self*/, void* /*closure*/) noexcept
{
    // warning that it is deprecated. See "create_new"
    if (PyErr_WarnEx(NULL, "private_attribute.register_metaclass is deprecated. See 'private_attribute.create_new'.", 2) < 0) {
        return NULL;
    }
    return PyCFunction_NewEx(&PrivateModule_methods[2], NULL, NULL);
}

static PyObject*
PrivateModule_get_ensure_type(PyObject* /*self*/, void* /*closure*/) noexcept
{
    return PyCFunction_NewEx(&PrivateModule_methods[3], NULL, NULL);
}

static PyObject*
PrivateModule_get_ensure_metaclass(PyObject* /*self*/, void* /*closure*/) noexcept
{
    return PyCFunction_NewEx(&PrivateModule_methods[4], NULL, NULL);
}

static PyObject*
PrivateModule_get_create_new(PyObject* /*self*/, void* /*closure*/) noexcept
{
    return PyCFunction_NewEx(&PrivateModule_methods[5], NULL, NULL);
}

static PyObject*
PrivateModule_get_create_meta_slot(PyObject* /*self*/, void* /*closure*/) noexcept
{
    return PyCFunction_NewEx(&PrivateModule_methods[6], NULL, NULL);
}

static PyObject*
PrivateModule_get_create_slot(PyObject* /*self*/, void* /*closure*/) noexcept
{
    return PyCFunction_NewEx(&PrivateModule_methods[7], NULL, NULL);
}

static PyGetSetDef PrivateModule_getsetters[] = {
    {"PrivateWrapProxy", (getter)PrivateModule_get_PrivateWrapProxy, NULL, NULL, NULL},
    {"PrivateAttrType", (getter)PrivateModule_get_PrivateAttrType, NULL, NULL, NULL},
    {"PrivateAttrBase", (getter)PrivateModule_get_PrivateAttrBase, NULL, NULL, NULL},
    {"PrivateSuper", (getter)PrivateModule_get_PrivateSuper, NULL, NULL, NULL},
    {"PrivateClassSuper", (getter)PrivateModule_get_PrivateClassSuper, NULL, NULL, NULL},
    {"prepare", (getter)PrivateModule_get_prepare, NULL, NULL, NULL},
    {"postprocess", (getter)PrivateModule_get_postprocess, NULL, NULL, NULL},
    {"register_metaclass", (getter)PrivateModule_get_register_metaclass, NULL, NULL, NULL},
    {"ensure_type", (getter)PrivateModule_get_ensure_type, NULL, NULL, NULL},
    {"ensure_metaclass", (getter)PrivateModule_get_ensure_metaclass, NULL, NULL, NULL},
    {"create_new", (getter)PrivateModule_get_create_new, NULL, NULL, NULL},
    {"create_meta_slot", (getter)PrivateModule_get_create_meta_slot, NULL, NULL, NULL},
    {"create_slot", (getter)PrivateModule_get_create_slot, NULL, NULL, NULL},
    {NULL}
};

static PyMethodDef PrivateModule_methods_def[] = {
    {"__dir__", (PyCFunction)PrivateModule_dir, METH_NOARGS, NULL},
    {NULL}  // Sentinel
};

static PyObject*
PrivateModule_getattro(PyObject* self, PyObject* name) noexcept
{
    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "attribute name must be a string");
        return NULL;
    }
    const char* name_cstr = PyUnicode_AsUTF8(name);
    if (!name_cstr) {
        return NULL;
    }
    PrivateModule_GETATTRO_CASE(PrivateWrapProxy);
    PrivateModule_GETATTRO_CASE(PrivateAttrType);
    PrivateModule_GETATTRO_CASE(PrivateAttrBase);
    PrivateModule_GETATTRO_CASE(PrivateSuper);
    PrivateModule_GETATTRO_CASE(PrivateClassSuper);
    PrivateModule_GETATTRO_CASE(prepare);
    PrivateModule_GETATTRO_CASE(postprocess);
    PrivateModule_GETATTRO_CASE(register_metaclass);
    PrivateModule_GETATTRO_CASE(ensure_type);
    PrivateModule_GETATTRO_CASE(ensure_metaclass);
    PrivateModule_GETATTRO_CASE(create_new);
    PrivateModule_GETATTRO_CASE(create_meta_slot);
    PrivateModule_GETATTRO_CASE(create_slot);

    return PyModule_Type.tp_getattro(self, name);
}

static PyTypeObject PrivateModuleType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute.private_attribute_module", //tp_name
    PyModule_Type.tp_basicsize + 8,               //tp_basicsize   size of module object + 8 bytes for basicsize to avoid changing attribute '__class__'
    0,                                            //tp_itemsize
    0,                                            //tp_dealloc
    0,                                            //tp_print
    0,                                            //tp_getattr
    0,                                            //tp_setattr
    0,                                            //tp_compare
    0,                                            //tp_repr
    0,                                            //tp_as_number
    0,                                            //tp_as_sequence
    0,                                            //tp_as_mapping
    0,                                            //tp_hash
    0,                                            //tp_call
    0,                                            //tp_str
    (getattrofunc)PrivateModule_getattro,         //tp_getattro
    (setattrofunc)PrivateModule_setattro,         //tp_setattro
    0,                                            //tp_as_buffer
    Py_TPFLAGS_DEFAULT,                           //tp_flags
    0,                                            //tp_doc
    0,                                            //tp_traverse
    0,                                            //tp_clear
    0,                                            //tp_richcompare
    0,                                            //tp_weaklistoffset
    0,                                            //tp_iter
    0,                                            //tp_iternext
    PrivateModule_methods_def,                    //tp_methods
    0,                                            //tp_members
    PrivateModule_getsetters,                     //tp_getset
    &PyModule_Type,                               //tp_base
};

static const char* module_doc = R"(
A module that provides a metaclass for creating classes with private attributes.
Private attributes are defined in the `__private_attrs__` sequence and are only visitable in class codes.
You can use the `PrivateAttrBase` as the base class to create classes with private attributes.
The attributes which are private are not on the instance's `__dict__` and cannot be accessed outside
but in the methods defined in class it is reachable.
Usage example:
```python
class MyClass(PrivateAttrBase):
    __private_attrs__ = ('_private_attr1',)
    def __init__(self):
        self._private_attr1 = 1

    @property
    def public_attr1(self):
        return self._private_attr1
```
)";

static int PrivateModule_exec(PyObject* m) noexcept;
static PyObject* PrivateModule_create(PyObject* spec, PyModuleDef* def) noexcept;

static PyModuleDef_Slot PrivateModule_slots[] = {
    {Py_mod_create, (void*)PrivateModule_create},
    {Py_mod_exec, (void*)PrivateModule_exec},
#if PY_VERSION_HEX >= 0x030D0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}
};

static PyModuleDef def = {
    PyModuleDef_HEAD_INIT,
    "private_attribute",
    module_doc,
    0,
    NULL,
    PrivateModule_slots,
    NULL,
    NULL,
    NULL
};

static PyObject*
AtExit_clean_storage(PyObject* /*self*/, PyObject* /*obj*/) noexcept
{
    clean_all_storages();
    Py_RETURN_NONE;
}

static PyMethodDef clean_storage = {
    "clean_storage",
    (PyCFunction)AtExit_clean_storage,
    METH_NOARGS,
    "Clean all storages"
};

static bool resigtered_clean_func = false;

static int
atexit_register_clean_func(void) noexcept
{
    if (resigtered_clean_func) {
        return 0;
    }
    PyObjectStorage atexit = PyObjectStorage::from_new_ref(PyImport_ImportModule("atexit"));
    if (!atexit) {
        return -1;
    }

    PyObjectStorage func = PyObjectStorage::from_new_ref(PyCFunction_New(&clean_storage, NULL));
    if (!func) {
        return -1;
    }

    PyObjectStorage result = PyObjectStorage::from_new_ref(PyObject_CallMethod(atexit, "register", "O", func.get()));

    if (!result) {
        return -1;
    }
    resigtered_clean_func = true;
    return 0;
}

static PyObject*
PrivateModule_create(PyObject* spec, PyModuleDef* /*def*/) noexcept
{
    PyObjectStorage name = PyObjectStorage::from_new_ref(PyObject_GetAttrString(spec, "name"));
    if (!name) {
        return NULL;
    }
    // Instantiate the module directly as an instance of PrivateModuleType
    // (a subclass of `module`) so that the object returned from
    // PyInit_private_attribute is genuinely a
    // `private_attribute.private_attribute_module` instance from the start,
    // rather than a plain module re-typed with Py_SET_TYPE afterwards.
    return PyObject_CallFunction((PyObject*)&PrivateModuleType, "O", name.get());
}

static int
PrivateModule_exec(PyObject* m) noexcept
{
    // Eagerly create PrivateAttrBase so that CPython's subtype_traverse /
    // subtype_clear pointers are captured at import time (see
    // PrivateAttrCaptureGuard and ::AllData::captured_subtype_*), instead of
    // waiting for the first lazy attribute access to the module.
    PyObjectStorage private_attr_base = PyObjectStorage::from_new_ref(PrivateModule_get_PrivateAttrBase(NULL, NULL));
    if (!private_attr_base) {
        return -1;
    }
    // add _PrivateWrap type
    if (PyModule_AddType(m, &PrivateWrapType) < 0) {
        return -1;
    }
    PyObject* all = Py_BuildValue("[sssssssssssss]",
        "PrivateWrapProxy",
        "PrivateAttrType",
        "PrivateAttrBase",
        "PrivateSuper",
        "PrivateClassSuper",
        "prepare",
        "postprocess",
        "register_metaclass",
        "ensure_type",
        "ensure_metaclass",
        "create_new",
        "create_meta_slot",
        "create_slot"
    );
    if (!all) {
        return -1;
    }
    if (PyModule_AddObject(m, "__all__", all) < 0) {
        Py_DECREF(all);
        return -1;
    }

    AllData::store_module_self[AllData::kModuleSelfKey] = m;    // store module self

    return 0;
}

PyMODINIT_FUNC
PyInit_private_attribute(void) noexcept
{
    // check if AllData::store_module_self already holds the module
    auto it = AllData::store_module_self.find(AllData::kModuleSelfKey);
    if (it != AllData::store_module_self.end()) {
        PyObject* m = it->second.get();
        Py_INCREF(m);
        return m;
    }
    if (init_all_slots() <0 ||
        atexit_register_clean_func() < 0 ||
        PyType_Ready(&PrivateAttrSlotType) < 0 ||
        PyType_Ready(&PrivateAttrMetaSlotType) < 0 ||
        PyType_Ready(&PrivateAttrMetaNewSlotType);
        PyType_Ready(&listproxyType) < 0 ||
        PyType_Ready(&PrivateWrapType) < 0 ||
        PyType_Ready(&PrivateWrapProxyType) < 0 ||
        PyType_Ready(&PrivateSuperType) < 0 ||
        PyType_Ready(&PrivateClassSuperType) < 0 ||
        PyType_Ready(&PrivateAttrType) < 0 ||
        PyType_Ready(&PrivateModuleType) < 0 ||
        PyType_Ready(&PrivateTempType) < 0) {
        return NULL;
    }
    return PyModuleDef_Init(&def);
}
