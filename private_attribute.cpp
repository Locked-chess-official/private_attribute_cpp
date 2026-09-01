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

// python under 3.13 doesn't have PyDict_ContainsString, so we implement it ourselves
#if PY_VERSION_HEX < 0x030D0000
static int
PyDict_ContainsString(PyObject *op, const char *key) noexcept
{
    PyObject *key_obj = PyUnicode_FromString(key);
    if (key_obj == NULL) {
        return -1;
    }
    int res = PyDict_Contains(op, key_obj);
    Py_DECREF(key_obj);
    return res;
}
#endif

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

    operator PyCodeObject*() const noexcept {
        return reinterpret_cast<PyCodeObject*>(obj);
    }
    PyObject* get() const noexcept {
        return obj;
    }
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
        static std::unordered_map<uintptr_t, std::shared_ptr<std::shared_mutex>> all_type_mutex;
        static std::unordered_map<uintptr_t, PyObjectStorage> type_need_call;
        static std::unordered_map<uintptr_t, std::unordered_set<TwoStringTuple>> all_type_attr_set;
        namespace {
            static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t,
                std::unordered_map<std::string, PyObjectStorage>>> all_object_attr, all_type_subclass_attr;
        };
        static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t, std::shared_ptr<std::shared_mutex>>>
            all_object_mutex, all_type_subclass_mutex;
        static std::mutex object_creatmutex_mutex, type_createmutex_mutex, type_parenttype_createmutex_mutex;
        static std::unordered_map<uintptr_t, std::vector<uintptr_t>> all_type_parent_id;
        // all type tp_getattro map
        static std::unordered_map<uintptr_t, getattrofunc> all_type_getattro;
        // all type tp_setattro map
        static std::unordered_map<uintptr_t, setattrofunc> all_type_setattro;
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
        static std::unordered_map<uintptr_t, destructor> all_type_del;

        static std::shared_mutex all_register_new_metaclass_mutex;
        static std::unordered_map<uintptr_t, PyObjectStorage> all_register_type_weak_ref;
        static std::vector<PyObjectStorage> store_module_self;
    };
};

static inline void
object_create_mutex(uintptr_t final_id, uintptr_t obj_id) noexcept
{
    // get object_creatmutex_mutex
    std::lock_guard<std::mutex> lock(AllData::object_creatmutex_mutex);
    if (::AllData::all_object_attr[final_id].find(obj_id) == ::AllData::all_object_attr[final_id].end()) {
        ::AllData::all_object_attr[final_id][obj_id] = {};
    }
    if (::AllData::all_object_mutex.find(final_id) == ::AllData::all_object_mutex.end()) {
        ::AllData::all_object_mutex[final_id] = {};
    }
    if (::AllData::all_object_mutex[final_id].find(obj_id) == ::AllData::all_object_mutex[final_id].end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_object_mutex[final_id][obj_id] = lock;
    }
}

static inline void
type_create_mutex(uintptr_t typ_id) noexcept {
    std::lock_guard<std::mutex> lock(AllData::type_createmutex_mutex);
    if (::AllData::type_attr_dict.find(typ_id) == ::AllData::type_attr_dict.end()) {
        ::AllData::type_attr_dict[typ_id] = {};
    }
    if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_type_mutex[typ_id] = lock;
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
        ::AllData::all_type_subclass_mutex[final_id] = {};
    }
    if (::AllData::all_type_subclass_mutex[final_id].find(typ_id) == ::AllData::all_type_subclass_mutex[final_id].end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_type_subclass_mutex[final_id][typ_id] = lock;
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
    AllData::all_type_mutex.clear();
    AllData::type_need_call.clear();
    AllData::all_type_attr_set.clear();
    AllData::all_object_attr.clear();
    AllData::all_type_subclass_attr.clear();
    AllData::all_object_mutex.clear();
    AllData::all_type_subclass_mutex.clear();
    AllData::all_type_parent_id.clear();
    AllData::all_type_getattro.clear();
    AllData::all_type_setattro.clear();
    AllData::all_type_traverse.clear();
    AllData::all_type_clear.clear();
    AllData::all_type_del.clear();
    AllData::all_register_type_weak_ref.clear();
    AllData::store_module_self.clear();
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
static uintptr_t type_set_attr_long_long_guidance(uintptr_t type, const std::string& name) noexcept;
static bool type_private_attr(uintptr_t type, const std::string& name) noexcept;
static FinalObject type_get_final_attr(uintptr_t type_id, const std::string& name) noexcept;

// Result of classifying an attribute access.
// - type_id: the node on the MRO chain (a class) whose code is currently running
//   AND that declares the attribute as private. 0 means "no private owner".
// - allowed: false means the access must be denied (a private attribute touched
//   from outside its declaring class, or from a subclass of it).
struct AttrClassifyResult
{
    uintptr_t type_id = 0;
    bool allowed = true;
};

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
    return false;
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
    return false;
}

static AttrClassifyResult
attr_classify(uintptr_t typ_id, const std::string& name, PyCodeObject* code) noexcept
{
    // Build the MRO chain: the actual class first, then its parents.
    std::vector<uintptr_t> chain;
    chain.push_back(typ_id);
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()) {
        auto& parent_ids = ::AllData::all_type_parent_id[typ_id];
        chain.insert(chain.end(), parent_ids.begin(), parent_ids.end());
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
                return {0, false};
            }
        }
        return {0, true};
    }

    if (is_type_private(code_class, name)) {
        // Both conditions hold: the code is under this class AND the attribute
        // is private to this very class -> allowed, and this node of the chain
        // is the owner of the private attribute.
        return {code_class, true};
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
            return {0, false};
        }
    }

    // Not private to the running class nor to any of its ancestors: treat it
    // as a normal (non-private) attribute access.
    return {0, true};
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

struct RestorePythonExceptionResult {
    std::string value;
    bool ok = true;
};

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
            PyObject* python_result = PyObject_CallFunction(
                func,
                "ns",
                static_cast<Py_ssize_t>(obj_id),
                attr_name.c_str()
            );
            if (python_result) {
                if (!PyUnicode_Check(python_result)) {
                    Py_DECREF(python_result);
                    PyErr_SetString(PyExc_TypeError, "private_func function must return a string");
                    result.ok = false;
                    result.value = "private_func function must return a string";
                    return result;
                }
                result.value = PyUnicode_AsUTF8(python_result);
                Py_DECREF(python_result);
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

static void ensure_tp(PyTypeObject* type_instance) noexcept;
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
    FinalObject final_object = type_get_final_attr((uintptr_t)typ, attr_name);
    if (final_object.status == -2) {
        return NULL;
    }

    std::string obj_private_name;
    std::string typ_private_name;
    PyObject* obj_need_call = NULL;
    if (::AllData::type_need_call.find(final_id) != ::AllData::type_need_call.end()) {
        obj_need_call = ::AllData::type_need_call[final_id];
    }
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
        ensure_tp((PyTypeObject*)typ);
        ensure_subclass_tp((PyTypeObject*)typ);
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
        ensure_tp((PyTypeObject*)typ);
        ensure_subclass_tp((PyTypeObject*)typ);
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
type_getattr(PyObject* typ, const std::string& attr_name) noexcept
{
    uintptr_t typ_id = (uintptr_t)typ;
    FinalObject final_object = type_get_final_attr(typ_id, attr_name);
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
            ensure_tp((PyTypeObject*)type);
            ensure_subclass_tp((PyTypeObject*)type);
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
    PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", type_name, attr_name.c_str());
    return NULL;
}

static int
id_setattr(uintptr_t final_id, const std::string& attr_name, PyObject* obj, PyObject* typ, PyObject* value) noexcept
{
    uintptr_t obj_id = (uintptr_t) obj;
    // Class-level fallback resolves per-subject (the instance's type).
    FinalObject final_object = type_get_final_attr((uintptr_t)typ, attr_name);
    if (final_object.status == -2) {
        return -1;
    }

    std::string obj_private_name;
    std::string typ_private_name;
    PyObject* obj_need_call = NULL;
    if (::AllData::type_need_call.find(final_id) != ::AllData::type_need_call.end()) {
        obj_need_call = ::AllData::type_need_call[final_id];
    }
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

static int type_delattr(PyObject* typ, const std::string& attr_name) noexcept;

static int
type_setattr(PyObject* typ, const std::string& attr_name, PyObject* value) noexcept
{
    if (!value) {
        return type_delattr(typ, attr_name);
    }
    uintptr_t typ_id = (uintptr_t) typ;
    uintptr_t final_id = type_set_attr_long_long_guidance(typ_id, attr_name);
    std::string final_key;
    PyObject* type_need_call;
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        type_need_call = ::AllData::type_need_call[typ_id];
    } else {
        type_need_call = NULL;
    }
    if (type_need_call) {
        auto private_name_result = custom_random_string(typ_id, attr_name, type_need_call);
        if (!private_name_result.ok) {
            return -1;
        }
        final_key = private_name_result.value;
    } else {
        final_key = default_random_string(typ_id, attr_name);
    }
    if (final_id == 0) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return -1;
    }
    if (final_id == typ_id) {
        type_create_mutex(typ_id);
        {
            std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
            ::AllData::type_attr_dict[typ_id][final_key] = value;
        }
        return 0;
    } else {
        // The accessed class is a subclass that stores its own value for a
        // parent's private name separately: all_type_subclass_attr[final_id][typ_id].
        type_parenttype_create_mutex(final_id, typ_id);
        {
            std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[final_id][typ_id]);
            ::AllData::all_type_subclass_attr[final_id][typ_id][final_key] = value;
            return 0;
        }
    }
}

static int
id_delattr(uintptr_t final_id, const std::string& attr_name, PyObject* obj, PyObject* typ) noexcept
{
    uintptr_t obj_id = (uintptr_t) obj;
    // Class-level fallback resolves per-subject (the instance's type).
    FinalObject final_object = type_get_final_attr((uintptr_t)typ, attr_name);
    if (final_object.status == -2) {
        return -1;
    }

    std::string obj_private_name;
    std::string typ_private_name;
    PyObject* obj_need_call = NULL;
    if (::AllData::type_need_call.find(final_id) != ::AllData::type_need_call.end()) {
        obj_need_call = ::AllData::type_need_call[final_id];
    }
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
            const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
            if (type_name == NULL) {
                return -1;
            }
            PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", type_name, attr_name.c_str());
        }
        ::AllData::all_object_attr[final_id][obj_id].erase(obj_private_name);
    }
    return 0;
}

static int
type_delattr(PyObject* typ, const std::string& attr_name) noexcept
{
    uintptr_t typ_id = (uintptr_t) typ;
    uintptr_t final_id = type_set_attr_long_long_guidance(typ_id, attr_name);
    std::string final_key;
    PyObject* type_need_call;
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        type_need_call = ::AllData::type_need_call[typ_id];
    } else {
        type_need_call = NULL;
    }
    if (type_need_call) {
        auto private_name_result = custom_random_string(typ_id, attr_name, type_need_call);
        if (!private_name_result.ok) {
            return -1;
        }
        final_key = private_name_result.value;
    } else {
        final_key = default_random_string(typ_id, attr_name);
    }
    if (final_id == 0) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return -1;
    }
    if (typ_id == final_id) {
        type_create_mutex(typ_id);
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
        if (::AllData::type_attr_dict[typ_id].find(final_key) == ::AllData::type_attr_dict[typ_id].end()) {
            const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
            if (type_name == NULL) {
                return -1;
            }
            PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%s'", type_name, attr_name.c_str());
            return -1;
        }
        ::AllData::type_attr_dict[typ_id].erase(final_key);
    } else {
        type_parenttype_create_mutex(final_id, typ_id);
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[final_id][typ_id]);
        if (::AllData::all_type_subclass_attr[final_id][typ_id].find(final_key) == ::AllData::all_type_subclass_attr[final_id][typ_id].end()) {
            const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
            if (type_name == NULL) {
                return -1;
            }
            PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%s'", type_name, attr_name.c_str());
            return -1;
        }
        ::AllData::all_type_subclass_attr[final_id][typ_id].erase(final_key);
    }
    return 0;
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

static PyMemberDef PrivateWrap_members[] = {
    {"result", T_OBJECT, offsetof(PrivateWrapObject, result), READONLY, "The result object"},
    {"__wrapped__", T_OBJECT, offsetof(PrivateWrapObject, result), READONLY, "The result object"},
    {NULL}
};

static PyObject *
PrivateWrap_funcs(PyObject *obj, void* /*closure*/) noexcept
{
    if (!obj) {
        Py_RETURN_NONE;
    }

    return PySequence_Tuple(((PrivateWrapObject*)obj)->func_list);
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

static const char* PrivateWrap_funcs_doc = "the original functions";

static PyGetSetDef PrivateWrap_getset[] = {
    {"funcs", (getter)PrivateWrap_funcs, NULL, PrivateWrap_funcs_doc, NULL},
    {"__doc__", (getter)PrivateWrap_doc, NULL, NULL, NULL},
    {"__module__", (getter)PrivateWrap_module, NULL, NULL, NULL},
    {"__name__", (getter)PrivateWrap_name, NULL, NULL, NULL},
    {"__qualname__", (getter)PrivateWrap_qualname, NULL, NULL, NULL},
    {"__annotate__", (getter)PrivateWrap_annotate, NULL, NULL, NULL},
    {"__type_params__", (getter)PrivateWrap_type_params, NULL, NULL, NULL},
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

static PyTypeObject PrivateWrapType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_PrivateWrap",                    // tp_name
    sizeof(PrivateWrapObject),         // tp_basicsize
    0,                                 // tp_itemsize
    (destructor)PrivateWrap_dealloc,   // tp_dealloc
    0,                                 // tp_print
    0,                                 // tp_getattr
    0,                                 // tp_setattr
    0,                                 // tp_reserved
    0,                                 // tp_repr
    0,                                 // tp_as_number
    0,                                 // tp_as_sequence
    0,                                 // tp_as_mapping
    0,                                 // tp_hash
    (ternaryfunc)PrivateWrap_call,     // tp_call
    0,                                 // tp_str
    PrivateWrap_getattro,              // tp_getattro
    0,                                 // tp_setattro
    0,                                 // tp_as_buffer
    Py_TPFLAGS_DEFAULT,                // tp_flags
    "private_attribute._PrivateWrap",  // tp_doc
    0,                                 // tp_traverse
    0,                                 // tp_clear
    0,                                 // tp_richcompare
    0,                                 // tp_weaklistoffset
    0,                                 // tp_iter
    0,                                 // tp_iternext
    0,                                 // tp_methods
    PrivateWrap_members,               // tp_members
    PrivateWrap_getset,                // tp_getset
    0,                                 // tp_base
    0,                                 // tp_dict
    PrivateWrap_descrget,              // tp_descr_get
    PrivateWrap_descrset,              // tp_descr_set
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
        PyObject_New(PrivateWrapObject, &PrivateWrapType);
    self->func_list = list;
    Py_INCREF(list);

    self->result = wrapped;

    return self;
}

static void
PrivateWrap_dealloc(PrivateWrapObject *self) noexcept
{
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
    if (!PyArg_ParseTuple(args, "O", &func)) return NULL;
    if(PyObject_TypeCheck(func, &PrivateWrapType)) {
        return (PyObject*)PrivateWrap_New(
            self->decorator,
            ((PrivateWrapObject*)func)->result,
            PySequence_Concat(((PrivateWrapObject*)func)->func_list,
                              self->func_list)
        );
    }

    PyObject *new_list = PyList_New(0);
    PyList_Append(new_list, func);

    PyObject *combined =
        PySequence_Concat(new_list, self->func_list);

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
    Py_TPFLAGS_DEFAULT,                     // tp_flags
    PrivateWrapProxy_doc,                   // tp_doc
    0,                                      // tp_traverse
    0,                                      // tp_clear
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

    if (!PyArg_ParseTuple(args, "O|O", &decorator, &orig))
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

// ===============================================================
// PrivateAttrType
// ===============================================================
typedef struct {
    PyHeapTypeObject base; // PyObject_HEAD_INIT(NULL)
} PrivateAttrTypeObject;

static void PrivateAttr_object_init_private_dict(uintptr_t obj_id, uintptr_t type_id) noexcept;
static int PrivateAttr_tp_setattro(PyObject* self, PyObject* name, PyObject* value) noexcept;
static void PrivateAttr_tp_finalize(PyObject* self) noexcept;
static void clear_object_related(uintptr_t id_self, uintptr_t typ_id) noexcept;

static PyObject*
PrivateAttr_tp_getattro(PyObject* self, PyObject* name) noexcept
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t type_id = (uintptr_t)typ;
    PrivateAttr_object_init_private_dict((uintptr_t)self, type_id);
    std::string name_str = PyUnicode_AsUTF8(name);
    auto code = get_now_code();
    if (type_private_attr(type_id, name_str)) {
        if (!code) {
            Py_XDECREF(code);
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            return NULL;
        }
        AttrClassifyResult classify = attr_classify(type_id, name_str, code);
        Py_XDECREF(code);
        if (!classify.allowed) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            return NULL;
        }
        if (classify.type_id != 0) {
            return id_getattr(classify.type_id, name_str, self, (PyObject*)typ);
        }
        // owner is 0: not private for this access context -> normal attribute
    }
    Py_XDECREF(code);
    if (::AllData::all_type_getattro.find(type_id) != ::AllData::all_type_getattro.end()){
        if (::AllData::all_type_getattro[type_id]) {
            PyObject* result = ::AllData::all_type_getattro[type_id](self, name);
            ensure_tp(typ);
            return result;
        }
    }
    return PyObject_GenericGetAttr(self, name);
}

static int
PrivateAttr_tp_setattro(PyObject* self, PyObject* name, PyObject* value) noexcept
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t typ_id = (uintptr_t)typ;
    const char* c_name = PyUnicode_AsUTF8(name);
    if (!c_name) {
        return -1;
    }
    std::string name_str(c_name);
    auto code = get_now_code();
    if (type_private_attr(typ_id, name_str)) {
        if (!code) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            Py_XDECREF(code);
            return -1;
        }
        AttrClassifyResult classify = attr_classify(typ_id, name_str, code);
        Py_XDECREF(code);
        if (!classify.allowed) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            return -1;
        }
        if (classify.type_id != 0) {
            if (!value) {
                return id_delattr(classify.type_id, name_str, self, (PyObject*)typ);
            }
            return id_setattr(classify.type_id, name_str, self, (PyObject*)typ, value);
        }
        // owner is 0: not private for this access context -> normal attribute
    }
    Py_XDECREF(code);
    if (::AllData::all_type_setattro.find(typ_id) != ::AllData::all_type_setattro.end()){
        int result = ::AllData::all_type_setattro[typ_id](self, name, value);
        ensure_tp(typ);
        return result;
    }
    return PyObject_GenericSetAttr(self, name, value);
}

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
            char* addr = (char*)self + mp->offset;
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
            char* addr = (char*)self + mp->offset;
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

static void
PrivateAttr_tp_finalize(PyObject* self) noexcept
{
    KeepPythonException _;
    uintptr_t id_self = (uintptr_t)self;
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t typ_id = (uintptr_t)typ;

    Py_ssize_t refcnt = Py_REFCNT(self);
    // Chain to the original tp_finalize if there is one.
    if (::AllData::all_type_del.find(typ_id) != ::AllData::all_type_del.end()) {
        destructor orig = ::AllData::all_type_del[typ_id];
        if (orig && orig != PrivateAttr_tp_finalize) {
            orig(self);
        }
    }
    if (Py_REFCNT(self) > refcnt) {
        return;
    }
    clear_object_related(id_self, typ_id);
}

static destructor
get_need_tp_finalize(PyTypeObject* cls) noexcept
{
    PyTypeObject* base = cls->tp_base;
    while (base) {
        if (base->tp_finalize != PrivateAttr_tp_finalize) {
            return base->tp_finalize;
        } else if (::AllData::all_type_del.find((uintptr_t)base) != ::AllData::all_type_del.end()) {
            if (::AllData::all_type_del[(uintptr_t)base]) {
                return ::AllData::all_type_del[(uintptr_t)base];
            }
            base = base->tp_base;
        } else {
            base = base->tp_base;
        }
    }
    return NULL;
}

// get_type_need_tp_* : the metaclass counterparts of get_need_tp_*.
// get_need_tp_*          -> find the original slots of *types created by* the
//                            metaclasses (wrapped with the instance-level
//                            PrivateAttr_tp_* / PrivateAttr_tp_getattro set).
// get_type_need_tp_*     -> find the original slots of the *metaclasses*
//                            themselves (wrapped with the metaclass-level
//                            PrivateAttrType_getattr / PrivateAttrType_setattr /
//                            PrivateAttrType_tp_traverse /
//                            PrivateAttrType_tp_clear).
static getattrofunc get_type_need_tp_getattro(PyTypeObject* cls) noexcept;
static setattrofunc get_type_need_tp_setattro(PyTypeObject* cls) noexcept;
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
    if (::AllData::all_object_mutex.find(type_id) == ::AllData::all_object_mutex.end()) {
        ::AllData::all_object_mutex[type_id] = {};
    }
    if (::AllData::all_object_attr.find(type_id) == ::AllData::all_object_attr.end()) {
        ::AllData::all_object_attr[type_id] = {};
    }
    if (::AllData::all_object_mutex[type_id].find(obj_id) == ::AllData::all_object_mutex[type_id].end()) {
        ::AllData::all_object_mutex[type_id][obj_id] = std::shared_ptr<std::shared_mutex>(new std::shared_mutex());
    }
    if (::AllData::all_object_attr[type_id].find(obj_id) == ::AllData::all_object_attr[type_id].end()) {
        ::AllData::all_object_attr[type_id][obj_id] = {};
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
    0,                                          // tp_call
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
    return PyTuple_Pack(2, PyUnicode_FromString(name1hash.c_str()), PyUnicode_FromString(name2hash.c_str()));
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
type_get_final_attr(uintptr_t type_id, const std::string& name) noexcept
{
    // type_id is the SUBJECT: the class whose attribute is being accessed.
    // The walk resolves per-subject: a subclass's separately stored value
    // (all_type_subclass_attr[parent][subclass]) shadows the parent's own
    // class-level value, satisfying class-level separate storage.
    TwoStringTuple hash_tuple = get_string_hash_tuple2(name);
    if (::AllData::all_type_attr_set.find(type_id) != ::AllData::all_type_attr_set.end()) {
        if (::AllData::all_type_attr_set[type_id].find(hash_tuple) != ::AllData::all_type_attr_set[type_id].end()) {
            PyObject* type_need_call = NULL;
            if (::AllData::type_need_call.find(type_id) != ::AllData::type_need_call.end()) {
                type_need_call = ::AllData::type_need_call[type_id];
            }
            std::string key;
            if (type_need_call != NULL) {
                auto private_name_result = custom_random_string(type_id, name, type_need_call);
                if (!private_name_result.ok) {
                    return -2; // -2 means exception
                }
                key = private_name_result.value;
            } else {
                key = default_random_string(type_id, name);
            }
            type_create_mutex(type_id);
            std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[type_id]);
            auto& item_set = ::AllData::type_attr_dict[type_id];
            if (item_set.find(key) != item_set.end()) {
                PyObject* obj = item_set[key];
                return obj;
            }
        }
    }
    std::vector<uintptr_t> now_visited = {type_id};
    if (::AllData::all_type_parent_id.find(type_id) != ::AllData::all_type_parent_id.end()) {
        auto& parent_ids = ::AllData::all_type_parent_id[type_id];
        for (auto& parent_id: parent_ids) {
            if (::AllData::all_type_attr_set.find(parent_id) != ::AllData::all_type_attr_set.end()) {
                auto& item_set = ::AllData::all_type_attr_set[parent_id];
                if (item_set.find(hash_tuple) != item_set.end()) {
                    if (::AllData::all_type_subclass_attr.find(parent_id) != ::AllData::all_type_subclass_attr.end()) {
                        auto& now_mro_dict = ::AllData::all_type_subclass_attr[parent_id];
                        for (auto& now_visited_id: now_visited) {
                            if (now_mro_dict.find(now_visited_id) != now_mro_dict.end()) {
                                std::string key;
                                if (::AllData::type_need_call.find(now_visited_id) != ::AllData::type_need_call.end()) {
                                    PyObject* func = ::AllData::type_need_call[now_visited_id];
                                    if (func != NULL) {
                                        auto private_name_result = custom_random_string(now_visited_id, name, func);
                                        if (!private_name_result.ok) {
                                            return -2; // -2 means exception
                                        }
                                        key = private_name_result.value;
                                    } else {
                                        key = default_random_string(now_visited_id, name);
                                    }
                                } else {
                                    key = default_random_string(now_visited_id, name);
                                }
                                type_parenttype_create_mutex(parent_id, now_visited_id);
                                std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[parent_id][now_visited_id]);
                                if (now_mro_dict[now_visited_id].find(key) != now_mro_dict[now_visited_id].end()) {
                                    PyObject* obj = now_mro_dict[now_visited_id][key];
                                    return obj;
                                }
                            }
                        }
                    }
                    std::string key;
                    if (::AllData::type_need_call.find(parent_id) != ::AllData::type_need_call.end()) {
                        PyObject* func = ::AllData::type_need_call[parent_id];
                        if (func != NULL) {
                            auto private_name_result = custom_random_string(parent_id, name, func);
                            if (!private_name_result.ok) {
                                return -2; // -2 means exception
                            }
                            key = private_name_result.value;
                        } else {
                            key = default_random_string(parent_id, name);
                        }
                    } else {
                        key = default_random_string(parent_id, name);
                    }
                    type_create_mutex(parent_id);
                    auto& item_set = ::AllData::type_attr_dict[parent_id];
                    std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[parent_id]);
                    if (item_set.find(key) != item_set.end()) {
                        PyObject* obj = item_set[key];
                        return obj;
                    }
                }
            }
            now_visited.push_back(parent_id);
        }
    }
    return -1; // -1 means not found
}

static uintptr_t
type_set_attr_long_long_guidance(uintptr_t type_id, const std::string& name) noexcept
{
    TwoStringTuple hash_tuple = get_string_hash_tuple2(name);
    if (::AllData::all_type_attr_set.find(type_id) != ::AllData::all_type_attr_set.end()) {
        auto& item_set = ::AllData::all_type_attr_set[type_id];
        if (item_set.find(hash_tuple) != item_set.end()) {
            return type_id;
        }
    }
    if (::AllData::all_type_parent_id.find(type_id) != ::AllData::all_type_parent_id.end()) {
        auto& parent_id_list = ::AllData::all_type_parent_id[type_id];
        for (auto& parent_id: parent_id_list) {
            auto& item_set = ::AllData::all_type_attr_set[parent_id];
            if (item_set.find(hash_tuple) != item_set.end()) {
                return parent_id;
            }
        }
    }
    return 0; // 0 means not found
}

static bool
type_private_attr(uintptr_t type_id, const std::string& name) noexcept
{
    TwoStringTuple hash_tuple = get_string_hash_tuple2(name);
    if (::AllData::all_type_attr_set.find(type_id) != ::AllData::all_type_attr_set.end()) {
        auto& item_set = ::AllData::all_type_attr_set[type_id];
        if (item_set.find(hash_tuple) != item_set.end()) {
            return true;
        }
    }
    if (::AllData::all_type_parent_id.find(type_id) != ::AllData::all_type_parent_id.end()) {
        auto& parent_id_list = ::AllData::all_type_parent_id[type_id];
        for (auto& parent_id: parent_id_list) {
            auto& item_set = ::AllData::all_type_attr_set[parent_id];
            if (item_set.find(hash_tuple) != item_set.end()) {
                return true;
            }
        }
    }
    return false;
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
        PyObject* co_contain = try_get_attr_string(obj, "co_consts");
        if (co_contain && PySequence_Check(co_contain)) {
            Py_ssize_t len = PySequence_Length(co_contain);
            if (len >= 0) {
                for (Py_ssize_t i = 0; i < len; i++) {
                    PyObject* item = PySequence_GetItem(co_contain, i);
                    if (item) {
                        analyse_all_code(item, map, _seen);
                        Py_DECREF(item);
                    }
                }
            }
        }
        Py_XDECREF(co_contain);
        return;
    }
    if (PyObject_TypeCheck(obj, &PrivateWrapType)) {
        PyObject* func_list = ((PrivateWrapObject*)obj)->func_list;
        if (func_list && PySequence_Check(func_list)) {
            Py_ssize_t len = PySequence_Length(func_list);
            if (len >= 0) {
                for (Py_ssize_t i = 0; i < len; i++) {
                    PyObject* func = PySequence_GetItem(func_list, i);
                    if (func) {
                        analyse_all_code(func, map, _seen);
                        Py_DECREF(func);
                    }
                }
            }
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PyProperty_Type)) {
        PyObject* fget = try_get_attr_string(obj, "fget");
        if (fget) {
            analyse_all_code(fget, map, _seen);
            Py_DECREF(fget);
        }
        PyObject* fset = try_get_attr_string(obj, "fset");
        if (fset) {
            analyse_all_code(fset, map, _seen);
            Py_DECREF(fset);
        }
        PyObject* fdel = try_get_attr_string(obj, "fdel");
        if (fdel) {
            analyse_all_code(fdel, map, _seen);
            Py_DECREF(fdel);
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PyClassMethod_Type) || PyObject_TypeCheck(obj, &PyStaticMethod_Type)) {
        PyObject* func = try_get_attr_string(obj, "__func__");
        if (func) {
            analyse_all_code(func, map, _seen);
            Py_DECREF(func);
        }
        return;
    }
    PyObject* wrap = try_get_attr_string(obj, "__wrapped__");
    if (wrap) {
        if (wrap == obj) {
            PyObject* code = try_get_attr_string(obj, "__code__");
            if (code) {
                analyse_all_code(code, map, _seen);
                Py_DECREF(code);
            }
            Py_DECREF(wrap);
            return;
        }
        analyse_all_code(wrap, map, _seen);
        Py_DECREF(wrap);
        return;
    }
    PyObject* code = try_get_attr_string(obj, "__code__");
    if (code) {
        analyse_all_code(code, map, _seen);
        Py_DECREF(code);
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

struct PrivateAttrCreationData
{
    std::string class_name;
    PyObject* attrs_copy = nullptr;
    PyObject* new_hash_private_attrs = nullptr;
    std::unordered_set<TwoStringTuple> private_attrs_set;
    std::unordered_set<std::string> private_attrs_vector_string;
    std::vector<uintptr_t> all_need_analyse_base;
    std::unordered_map<std::string, PyObjectStorage> need_remove_itself;
    // Class-body attributes whose name is private to a parent class: stored
    // separately in all_type_subclass_attr[parent][this class].
    std::unordered_map<uintptr_t, std::unordered_map<std::string, PyObjectStorage>> need_remove_subclass;
    PyObject* private_func = nullptr;
    PyObject* base_kwds = nullptr;
    PyObject* name = nullptr;
    PyObject* bases = nullptr;
    PyObject* attrs = nullptr;
    bool cleared = false;

    void clear() noexcept {
        if (cleared) {
            return;
        }
        if (name) {
            Py_DECREF(name);
            name = nullptr;
        }
        if (bases) {
            Py_DECREF(bases);
            bases = nullptr;
        }
        if (attrs) {
            Py_DECREF(attrs);
            attrs = nullptr;
        }
        if (attrs_copy) {
            Py_DECREF(attrs_copy);
            attrs_copy = nullptr;
        }
        if (new_hash_private_attrs) {
            Py_DECREF(new_hash_private_attrs);
            new_hash_private_attrs = nullptr;
        }
        if (private_func) {
            Py_DECREF(private_func);
            private_func = nullptr;
        }
        if (base_kwds) {
            Py_DECREF(base_kwds);
            base_kwds = nullptr;
        }

        need_remove_itself.clear();
        need_remove_subclass.clear();
        cleared = true;
    }

    ~PrivateAttrCreationData() noexcept {
        clear();
    }
};

static bool
need_analyse_type(PyObject* type) noexcept
{
    if (PyObject_IsInstance(type, (PyObject*)&PrivateAttrType)) {
        return true;
    }
    std::shared_lock lock(::AllData::all_register_new_metaclass_mutex);
    for (auto& [id, metaclassref]: ::AllData::all_register_type_weak_ref){
        if (!PyWeakref_CheckRef((PyObject*)metaclassref)) {
            continue;
        }
#if PY_VERSION_HEX < 0x030D0000
        PyObject* metaclass = PyWeakref_GET_OBJECT((PyObject*)metaclassref);
        if (type == metaclass) continue;
        if (PyObject_IsInstance(type, metaclass)) {
            return true;
        }
#else
        PyObject* metaclass;
        if (PyWeakref_GetRef(metaclassref, &metaclass) == 1) {
            if (type == metaclass) {
                Py_DECREF(metaclass);
                continue;
            }
            if (PyObject_IsInstance(type, metaclass)) {
                Py_DECREF(metaclass);
                return true;
            }
            Py_DECREF(metaclass);
        }
#endif
    }
    return false;
}

static bool
PrivateAttrType_preprocess(PyObject* args, PyObject* kwds, PrivateAttrCreationData& data) noexcept
{
    static const char* invalid_name[] = {"__private_attrs__", "__slots__", "__getattribute__", "__getattr__", "__init__",
        "__setattr__", "__delattr__", "__name__", "__module__", "__doc__", "__getstate__", "__setstate__",
        "__get__", "__set__", "__delete__", "__new__", "__set_name__", "__class__", NULL};

    if (!args) {
        PyErr_Format(PyExc_SystemError, "%s (%d): args is NULL", __FILE__, __LINE__);
        return false;
    }

    // only parse name, bases, attrs
    if (!PyArg_ParseTuple(args, "OOO", &data.name, &data.bases, &data.attrs)) {
        return false;
    }
    Py_INCREF(data.name);
    Py_INCREF(data.bases);
    Py_INCREF(data.attrs);

    if (!PyUnicode_Check(data.name)) {
        PyErr_SetString(PyExc_TypeError, "name must be a string");
        return false;
    }
    data.class_name = PyUnicode_AsUTF8(data.name);

    if (!PyTuple_Check(data.bases)) {
        PyErr_SetString(PyExc_TypeError, "bases must be a tuple");
        return false;
    }

    if (!PyAnyDict_Check(data.attrs)) {
        PyErr_SetString(PyExc_TypeError, "attrs must be a dict");
        return false;
    }

    PyObjectStorage __private_attrs__;
    if (PyDict_ContainsString(data.attrs, "__private_attrs__")) {
        __private_attrs__ = PyDict_GetItemString(data.attrs, "__private_attrs__");
    }
    else {
        __private_attrs__ = PyTuple_New(0);
        Py_DECREF(__private_attrs__);
    }

    if (PyUnicode_Check(__private_attrs__)) {
        PyObject* new___private_attrs__ = PyTuple_New(1);
        if (!new___private_attrs__) {
            return false;
        }
        PyTuple_SET_ITEM(new___private_attrs__, 0, __private_attrs__);
        __private_attrs__ = new___private_attrs__;
        Py_DECREF(__private_attrs__);
    }

    if (!PySequence_Check(__private_attrs__)) {
        PyErr_SetString(PyExc_TypeError, "'__private_attrs__' must be a sequence");
        return false;
    }

    data.attrs_copy = PyDict_Copy(data.attrs);
    if (!data.attrs_copy) {
        return false;
    }

    Py_ssize_t private_attr_len = PySequence_Length(__private_attrs__);
    if (private_attr_len < 0) {
        return false;
    }

    data.new_hash_private_attrs = PyTuple_New(private_attr_len);
    if (!data.new_hash_private_attrs) {
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
        PyTuple_SET_ITEM(data.new_hash_private_attrs, i, hash_tuple);
        data.private_attrs_set.insert(hash_tuple_key);
        data.private_attrs_vector_string.insert(attr_str);
    }

    if (PyDict_SetItemString(data.attrs_copy, "__private_attrs__", data.new_hash_private_attrs) < 0) {
        return false;
    }

    int has_slots = PyDict_ContainsString(data.attrs_copy, "__slots__");

    if (has_slots) {
        PyObject* all_slots = PyDict_GetItemString(data.attrs_copy, "__slots__");
        if (!all_slots) {return false;}
        if (PyUnicode_Check(all_slots)) {
            const char* slot_cstr = PyUnicode_AsUTF8(all_slots);
            if (data.private_attrs_vector_string.find((std::string)slot_cstr) != data.private_attrs_vector_string.end()){
                std::string error_msg = "'__slots__' and '__private_attrs__' cannot have the same attribute name: '" + std::string(slot_cstr) + "'";
                PyErr_SetString(PyExc_TypeError, error_msg.c_str());
                return false;
            }
        } else {
            PyObject* slot_seq;
            if (PyAnyDict_Check(all_slots)) {
                // use the key of the dict as the slot name
                PyObject* keys = PyDict_Keys(all_slots);
                if (!keys) {
                    return false;
                }
                slot_seq = keys;
            } else {
                slot_seq = PySequence_Fast(all_slots, "__slots__ must be a string, dict or sequence");
            }
            if (!slot_seq) {
                return false;
            }

            Py_ssize_t slot_len = PySequence_Fast_GET_SIZE(slot_seq);

            for (Py_ssize_t j = 0; j < slot_len; j++) {
                PyObject* slot = PySequence_Fast_GET_ITEM(slot_seq, j);
                if (PyUnicode_Check(slot)) {
                    const char* slot_cstr = PyUnicode_AsUTF8(slot);
                    std::string final_name = real_class_name(slot_cstr, data.class_name);
                    if (data.private_attrs_vector_string.find(final_name) != data.private_attrs_vector_string.end()){
                        PyErr_Format(PyExc_TypeError, "'__slots__' and '__private_attrs__' cannot have the same attribute name: '%s'", final_name.c_str());
                        Py_DECREF(slot_seq);
                        return false;
                    }
                }
            }
            Py_DECREF(slot_seq);
        }
    }

    Py_ssize_t bases_len = PyTuple_GET_SIZE(data.bases);
    for (Py_ssize_t i = 0; i < bases_len; i++) {
        PyObject* base = PyTuple_GET_ITEM(data.bases, i);
        if (!base || !PyType_Check(base) || !need_analyse_type(base)) {
            continue;
        }
        data.all_need_analyse_base.push_back((uintptr_t)base);
    }

    std::function<uintptr_t(std::string)> need_remove_to_subclass = [&data](std::string attr_name){
        for (auto& base: data.all_need_analyse_base) {
            if (type_private_attr(base, attr_name)) {
                return type_set_attr_long_long_guidance(base, attr_name);
            }
        }
        return (uintptr_t)0;
    };

    // forbidden to have "__static_attributes__" in attrs, because it exposes the private attributes name.
    if (PyDict_ContainsString(data.attrs_copy, "__static_attributes__")) {
        PyDict_DelItemString(data.attrs_copy, "__static_attributes__");
    }

    {
        Py_ssize_t pos = 0;
        PyObject* key, *value;
        PyObject* forward_analyse = PyDict_Copy(data.attrs_copy);
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
                PyDict_DelItem(data.attrs_copy, key);
                continue;
            }
            uintptr_t need_remove_subclass_id = need_remove_to_subclass(attr_name);
            if (need_remove_subclass_id) {
                // The class body defines an attribute whose name is private to a
                // parent class. It is stored separately (class-level separate
                // storage) in all_type_subclass_attr[need_remove_subclass_id][this].
                if (data.need_remove_subclass.find(need_remove_subclass_id) == data.need_remove_subclass.end()) {
                    data.need_remove_subclass[need_remove_subclass_id] = {};
                }
                // PyObjectStorage assignment INCREFs (RAII); no manual INCREF.
                data.need_remove_subclass[need_remove_subclass_id][attr_name] = need_value;
                PyDict_DelItem(data.attrs_copy, key);
                continue;
            }
            if (value != need_value) PyDict_SetItem(data.attrs_copy, key, need_value);
        }
        Py_DECREF(forward_analyse);
    }

    // get "private_func" from kwds
    if (kwds && PyDict_Check(kwds)) {
        bool has_private_func = PyDict_ContainsString(kwds, "private_func");
        if (has_private_func) {
            data.private_func = PyDict_GetItemString(kwds, "private_func");
            Py_INCREF(data.private_func);
        }
        data.base_kwds = PyDict_Copy(kwds);
        if (!data.base_kwds) {
            return false;
        }
        if (has_private_func) PyDict_DelItemString(data.base_kwds, "private_func");
    }

    return true;
}

static PyObject*
PrivateAttrType_create(PyTypeObject* type, PrivateAttrCreationData& data) noexcept
{
    PyObject* type_args = PyTuple_Pack(3, data.name, data.bases, data.attrs_copy);
    if (!type_args) {
        return nullptr;
    }

    PyObject* new_type = PyType_Type.tp_new(type, type_args, data.base_kwds);
    Py_DECREF(type_args);

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

static getattrofunc get_need_tp_getattro(PyTypeObject* cls) noexcept;
static setattrofunc get_need_tp_setattro(PyTypeObject* cls) noexcept;
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
    {
        if (type_instance->tp_getattro != PrivateAttr_tp_getattro) {
            if (!type_instance->tp_getattro && ::AllData::all_type_getattro.find(type_id) == ::AllData::all_type_getattro.end()) {
                ::AllData::all_type_getattro[type_id] = get_need_tp_getattro(type_instance);
            } else {
                ::AllData::all_type_getattro[type_id] = type_instance->tp_getattro;
            }
            type_instance->tp_getattro = PrivateAttr_tp_getattro;
        } else if (::AllData::all_type_getattro.find(type_id) == ::AllData::all_type_getattro.end()) {
            PyTypeObject* base = type_instance->tp_base;
            uintptr_t base_id = (uintptr_t)(base);
            if (::AllData::all_type_getattro.find(base_id) != ::AllData::all_type_getattro.end()) {
                ::AllData::all_type_getattro[type_id] = ::AllData::all_type_getattro[base_id];
            } else if (base && base->tp_getattro && base->tp_getattro != PrivateAttr_tp_getattro) {
                ::AllData::all_type_getattro[type_id] = base->tp_getattro;
            }
        }
    }
    {
        if (type_instance->tp_setattro != PrivateAttr_tp_setattro) {
            if (!type_instance->tp_setattro && ::AllData::all_type_setattro.find(type_id) == ::AllData::all_type_setattro.end()) {
                ::AllData::all_type_setattro[type_id] = get_need_tp_setattro(type_instance);
            } else {
                ::AllData::all_type_setattro[type_id] = type_instance->tp_setattro;
            }
            type_instance->tp_setattro = PrivateAttr_tp_setattro;
        } else if (::AllData::all_type_setattro.find(type_id) == ::AllData::all_type_setattro.end()){
            PyTypeObject* base = type_instance->tp_base;
            uintptr_t base_id = (uintptr_t)(base);
            if (::AllData::all_type_setattro.find(base_id) != ::AllData::all_type_setattro.end()) {
                ::AllData::all_type_setattro[type_id] = ::AllData::all_type_setattro[base_id];
            } else if (base && base->tp_setattro && base->tp_setattro != PrivateAttr_tp_setattro) {
                ::AllData::all_type_setattro[type_id] = base->tp_setattro;
            }
        }
    }
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
    {
        if (type_instance->tp_finalize != PrivateAttr_tp_finalize) {
            if (!type_instance->tp_finalize && ::AllData::all_type_del.find(type_id) == ::AllData::all_type_del.end()) {
                ::AllData::all_type_del[type_id] = get_need_tp_finalize(type_instance);
            } else {
                ::AllData::all_type_del[type_id] = type_instance->tp_finalize;
            }
            type_instance->tp_finalize = PrivateAttr_tp_finalize;
        }
    }
}

static void
ensure_subclass_tp(PyTypeObject* type_instance) noexcept
{
    KeepPythonException _;
    PyObject* subclasses = PyObject_CallMethod((PyObject*)&PyType_Type, "__subclasses__", "O", (PyObject*)type_instance);
    if (!subclasses) {
        PyObject* unraisable_missing = PyUnicode_FromString("type.__subclasses__()");
        PyErr_WriteUnraisable(unraisable_missing);
        Py_DECREF(unraisable_missing);
        return;
    }
    if (!PyList_Check(subclasses)) {
        PyErr_SetString(PyExc_TypeError, "type.__subclasses__() did not return a list");
        PyObject* unraisable_missing = PyUnicode_FromString("type.__subclasses__()");
        PyErr_WriteUnraisable(unraisable_missing);
        Py_DECREF(unraisable_missing);
        Py_DECREF(subclasses);
        return;
    }
    Py_ssize_t list_len = PyList_GET_SIZE(subclasses);
    for (Py_ssize_t i = 0; i < list_len; i++) {
        PyObject* subclass = PyList_GetItem(subclasses, i);
        if (!PyType_Check(subclass)) continue;
        ensure_tp((PyTypeObject*)subclass);
    }
    Py_DECREF(subclasses);
}

static bool
PrivateAttrType_postprocess(PyObject* new_type, PrivateAttrCreationData& data) noexcept
{
    if (!new_type) {
        return false;
    }
    if (!PyType_Check(new_type)) {
        PyErr_SetString(PyExc_TypeError, "created object is not a type");
        return false;
    }

    PyTypeObject* type_instance = (PyTypeObject*)new_type;
    uintptr_t type_id = (uintptr_t)(type_instance);
    if (PyDict_ContainsString(type_instance->tp_dict, "__static_attributes__")) {
        PyErr_Format(PyExc_SystemError,
            "%s (%d): '__static_attributes__' will expose private attribute names and cannot be used in private attribute types", __FILE__, __LINE__);
        return false;
    }

    ensure_tp(type_instance);

    ::AllData::type_attr_dict[type_id] = {};
    ::AllData::all_type_attr_set[type_id] = data.private_attrs_set;

    // iter mro and put in all_type_parent_id
    PyObject* mro = type_instance->tp_mro;
    Py_ssize_t mro_size = PyTuple_GET_SIZE(mro);
    std::vector<uintptr_t> mro_vector;
    for (Py_ssize_t i = 1; i < mro_size; i++) {
        PyObject* item = PyTuple_GET_ITEM(mro, i);
        if (!item || !PyType_Check(item) || !need_analyse_type(item)) {
            continue;
        }
        mro_vector.push_back((uintptr_t)item);
    }
    ::AllData::all_type_parent_id[type_id] = mro_vector;

    ::AllData::type_allowed_code_map[type_id] = {};
    ::AllData::all_object_mutex[type_id] = {};
    ::AllData::all_type_mutex[type_id] = std::make_shared<std::shared_mutex>();
    ::AllData::all_object_attr[type_id] = {};
    ::AllData::all_type_subclass_attr[type_id] = {};
    ::AllData::all_type_subclass_mutex[type_id] = {};

    for (uintptr_t i: mro_vector) {
        if (::AllData::all_type_subclass_attr.find(i) == ::AllData::all_type_subclass_attr.end()) {
            ::AllData::all_type_subclass_attr[i] = {};
        }
        if (::AllData::all_type_subclass_mutex.find(i) == ::AllData::all_type_subclass_mutex.end()) {
            ::AllData::all_type_subclass_mutex[i] = {};
        }
        ::AllData::all_type_subclass_attr[i][type_id] = {};
        ::AllData::all_type_subclass_mutex[i][type_id] = std::make_shared<std::shared_mutex>();
    }

    for (auto& [key, value]: data.need_remove_itself) {
        std::string final_key;
        if (data.private_func) {
            auto private_name_result = custom_random_string(type_id, key, data.private_func);
            if (!private_name_result.ok) {
                return false;
            }
            final_key = private_name_result.value;
        } else {
            final_key = default_random_string(type_id, key);
        }
        ::AllData::type_attr_dict[type_id][final_key] = value;
    }

    for (auto& [i, map]: data.need_remove_subclass) {
        if (::AllData::all_type_subclass_attr.find(i) == ::AllData::all_type_subclass_attr.end()) {
            ::AllData::all_type_subclass_attr[i] = {};
        }
        if (::AllData::all_type_subclass_mutex.find(i) == ::AllData::all_type_subclass_mutex.end()) {
            ::AllData::all_type_subclass_mutex[i] = {};
        }
        if (::AllData::all_type_subclass_attr[i].find(type_id) == ::AllData::all_type_subclass_attr[i].end()) {
            ::AllData::all_type_subclass_attr[i][type_id] = {};
        }
        if (::AllData::all_type_subclass_mutex[i].find(type_id) == ::AllData::all_type_subclass_mutex[i].end()) {
            ::AllData::all_type_subclass_mutex[i][type_id] = std::make_shared<std::shared_mutex>();
        }
        for (auto& [key, value]: map) {
            std::string final_key;
            if (data.private_func) {
                auto private_name_result = custom_random_string(type_id, key, data.private_func);
                if (!private_name_result.ok) {
                    return false;
                }
                final_key = private_name_result.value;
            } else {
                final_key = default_random_string(type_id, key);
            }
            ::AllData::all_type_subclass_attr[i][type_id][final_key] = value;
        }
    }

    if (data.private_func) {
        Py_INCREF(data.private_func);
        ::AllData::type_need_call[type_id] = data.private_func;
    }

    {
        PyObject* original_key;
        Py_ssize_t original_pos = 0;
        PyObject* original_value;
        while (PyDict_Next(data.attrs, &original_pos, &original_key, &original_value)) {
            std::unordered_set<uintptr_t> set;
            analyse_all_code(original_value, ::AllData::type_allowed_code_map[type_id], set);
        }
    }

    return true;
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

    if (!PrivateAttrType_preprocess(args, kwds, data)) {
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

    if (!PrivateAttrType_postprocess(new_type, data)) {
        Py_DECREF(new_type);
        return nullptr;
    }

    return new_type;
}

static PyObject*
PrivateAttrType_getattr(PyObject* cls, PyObject* name) noexcept
{
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "cls must be a type");
        return NULL;
    }
    uintptr_t typ_id = (uintptr_t)(cls);
    std::string name_str = PyUnicode_AsUTF8(name);
    PyCodeObject* now_code = get_now_code();
    if (type_private_attr(typ_id, name_str)) {
        if (!now_code) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            Py_XDECREF(now_code);
            return NULL;
        }
        AttrClassifyResult classify = attr_classify(typ_id, name_str, now_code);
        Py_XDECREF(now_code);
        if (!classify.allowed) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            return NULL;
        }
        if (classify.type_id != 0) {
            return type_getattr(cls, name_str);
        }
        // owner is 0: not private for this access context -> normal attribute
    }
    Py_XDECREF(now_code);
    PyTypeObject* base = Py_TYPE(cls)->tp_base;
    while (base && base->tp_base && base->tp_getattro == PrivateAttrType_getattr) {
        base = base->tp_base;
    }
    if (!base) {
        PyObject* result = PyType_Type.tp_getattro(cls, name);
        return result;
    }
    PyObject* result = base->tp_getattro(cls, name);
    return result;
}

static setattrofunc original_type_tp_setattro = 0;

static bool
is_registed_type(PyObject* type) noexcept
{
    uintptr_t typ_id = (uintptr_t)type;
    return (AllData::all_register_type_weak_ref.find(typ_id) != AllData::all_register_type_weak_ref.end());
}

static PyObject* ensure_metaclass_tp(PyObject* /*self*/, PyObject* metaclass) noexcept;

static int
new_type_tp_setattro(PyObject* self, PyObject* name, PyObject* value) noexcept
{
    std::vector<PyObject*> need_analyse_types_and_metaclasses;
    std::vector<PyObject*> registered_types_and_metaclasses;
    std::vector<PyObject*> all_subclasses;
    PyObject* subclasses = PyObject_CallMethod((PyObject*)&PyType_Type, "__subclasses__", "O", self);
    if (!subclasses) return -1;
    if (!PyList_Check(subclasses)) {
        PyErr_SetString(PyExc_TypeError, "subclasses of this class is not list");
        Py_DECREF(subclasses);
        return -1;
    }
    Py_ssize_t list_len = PyList_GET_SIZE(subclasses);
    for (Py_ssize_t i = 0; i < list_len; i++) {
        all_subclasses.push_back(PyList_GET_ITEM(subclasses, i));
    }
    if (need_analyse_type(self)) {
        need_analyse_types_and_metaclasses.push_back(self);
    } else if (is_registed_type(self)) {
        registered_types_and_metaclasses.push_back(self);
    }
    for (auto& i: all_subclasses) {
        if (need_analyse_type(i)) {
            need_analyse_types_and_metaclasses.push_back(i);
        } else if (is_registed_type(i)) {
            registered_types_and_metaclasses.push_back(i);
        }
    }
    int result = original_type_tp_setattro(self, name, value);
    for (auto& i: need_analyse_types_and_metaclasses) {
        ensure_tp((PyTypeObject*)i);
    }
    for (auto& i: registered_types_and_metaclasses) {
        ensure_metaclass_tp(0, i);
    }
    Py_DECREF(subclasses);
    return result;
}

static int
init_all_slots() noexcept
{
    original_type_tp_setattro = PyType_Type.tp_setattro;
    PyObject* original_setattr = PyObject_GetAttrString((PyObject*)&PyType_Type, "__setattr__");
    if (!original_setattr) {return -1;}
    if (original_setattr && PyObject_IsInstance(original_setattr, (PyObject*)&PyWrapperDescr_Type)) {
        ((PyWrapperDescrObject*)original_setattr)->d_wrapped = (void*)new_type_tp_setattro;
    }
    Py_DECREF(original_setattr);
    PyObject* original_delattr = PyObject_GetAttrString((PyObject*)&PyType_Type, "__delattr__");
    if (!original_delattr) {return -1;}
    if (original_delattr && PyObject_IsInstance(original_delattr, (PyObject*)&PyWrapperDescr_Type)) {
        ((PyWrapperDescrObject*)original_delattr)->d_wrapped = (void*)new_type_tp_setattro;
    }
    Py_DECREF(original_delattr);
    PyType_Type.tp_setattro = new_type_tp_setattro;
    PyObject* all_metaclasses = PyObject_CallMethod((PyObject*)&PyType_Type, "__subclasses__", "O", (PyObject*)&PyType_Type);
    if (!all_metaclasses) {return -1;}
    if (!PyList_Check(all_metaclasses)) {
        Py_DECREF(all_metaclasses);
        return 0;
    }
    Py_ssize_t list_len = PyList_GET_SIZE(all_metaclasses);
    for (Py_ssize_t i = 0; i < list_len; i++) {
        PyObject* metaclass = PyList_GET_ITEM(all_metaclasses, i);
        if (!PyType_Check(metaclass)) {continue;}
        if (((PyTypeObject*)metaclass)->tp_setattro == original_type_tp_setattro) {
            ((PyTypeObject*)metaclass)->tp_setattro = new_type_tp_setattro;
        }
    }
    Py_DECREF(all_metaclasses);
    return 0;
}

static getattrofunc
get_need_tp_getattro(PyTypeObject* cls) noexcept
{
    PyTypeObject* base = cls->tp_base;
    while (base) {
        if (base->tp_getattro != PrivateAttr_tp_getattro) {
            return base->tp_getattro;
        } else if (::AllData::all_type_getattro.find((uintptr_t)base) != ::AllData::all_type_getattro.end()) {
            if (::AllData::all_type_getattro[(uintptr_t)base]) {
                return ::AllData::all_type_getattro[(uintptr_t)base];
            }
            base = base->tp_base;
        } else {
            base = base->tp_base;
        }
    }
    return NULL;
}

static setattrofunc
get_need_tp_setattro(PyTypeObject* cls) noexcept
{
    PyTypeObject* base = cls->tp_base;
    while (base) {
        if (base->tp_setattro != PrivateAttr_tp_setattro) {
            return base->tp_setattro;
        } else if (::AllData::all_type_setattro.find((uintptr_t)base) != ::AllData::all_type_setattro.end()) {
            if (::AllData::all_type_setattro[(uintptr_t)base]) {
                return ::AllData::all_type_setattro[(uintptr_t)base];
            }
            base = base->tp_base;
        } else {
            base = base->tp_base;
        }
    }
    return NULL;
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

// ========================================================================
// get_type_need_tp_* : find the ORIGINAL slot implementation for METACLASS
// slots.
//
// Metaclass objects (registered through register_metaclass / kept in shape by
// ensure_metaclass) have their own slots replaced with the *metaclass-level*
// wrappers:
//   tp_getattro -> PrivateAttrType_getattr
//   tp_setattro -> PrivateAttrType_setattr
//   tp_traverse -> PrivateAttrType_tp_traverse
//   tp_clear    -> PrivateAttrType_tp_clear
//
// These helpers walk the metaclass hierarchy (cls->tp_base chain) and skip
// those wrappers, returning the nearest real implementation (either read
// directly from a base slot or from the saved ::AllData::all_type_* map).
//
// They are the metaclass counterparts of get_need_tp_*, which instead deal
// with the slots of the *types created by* these metaclasses (wrapped with
// the instance-level PrivateAttr_tp_* / PrivateAttr_tp_getattro set).
// ========================================================================
static getattrofunc
get_type_need_tp_getattro(PyTypeObject* cls) noexcept
{
    PyTypeObject* base = cls->tp_base;
    while (base) {
        if (base->tp_getattro != PrivateAttrType_getattr) {
            return base->tp_getattro;
        } else if (::AllData::all_type_getattro.find((uintptr_t)base) != ::AllData::all_type_getattro.end()) {
            if (::AllData::all_type_getattro[(uintptr_t)base]) {
                return ::AllData::all_type_getattro[(uintptr_t)base];
            }
            base = base->tp_base;
        } else {
            base = base->tp_base;
        }
    }
    return NULL;
}

static setattrofunc
get_type_need_tp_setattro(PyTypeObject* cls) noexcept
{
    PyTypeObject* base = cls->tp_base;
    while (base) {
        if (base->tp_setattro != PrivateAttrType_setattr) {
            return base->tp_setattro;
        } else if (::AllData::all_type_setattro.find((uintptr_t)base) != ::AllData::all_type_setattro.end()) {
            if (::AllData::all_type_setattro[(uintptr_t)base]) {
                return ::AllData::all_type_setattro[(uintptr_t)base];
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
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "cls must be a type");
        return -1;
    }
    // check Py_TPFLAGS_IMMUTABLETYPE
    uintptr_t typ_id = (uintptr_t)(cls);
    std::string name_str = PyUnicode_AsUTF8(name);
    if (((PyTypeObject*)cls)->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) {
        std::string error_message = "cannot set '" + name_str + "' attribute of immutable type '" + ((PyTypeObject*)cls)->tp_name + "'";
        PyErr_SetString(PyExc_TypeError, error_message.c_str());
        return -1;
    }
    PyCodeObject* now_code = get_now_code();
    if (type_private_attr(typ_id, name_str)) {
        if (!now_code) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            Py_XDECREF(now_code);
            return -1;
        }
        AttrClassifyResult classify = attr_classify(typ_id, name_str, now_code);
        Py_XDECREF(now_code);
        if (!classify.allowed) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            return -1;
        }
        if (classify.type_id != 0) {
            return type_setattr(cls, name_str, value);
        }
        // owner is 0: not private for this access context -> normal attribute
    }
    Py_XDECREF(now_code);
    PyTypeObject* base = Py_TYPE(cls)->tp_base;
    while (base && base->tp_base && base->tp_setattro == PrivateAttrType_setattr) {
        base = base->tp_base;
    }
    if (!base) {
        return new_type_tp_setattro(cls, name, value);
    }
    int result = base->tp_setattro(cls, name, value);
    ensure_tp((PyTypeObject*)cls);
    ensure_subclass_tp((PyTypeObject*)cls);
    return result;
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
    if (::AllData::all_type_getattro.find(typ_id) != ::AllData::all_type_getattro.end()) {
        ::AllData::all_type_getattro.erase(typ_id);
    }
    if (::AllData::all_type_setattro.find(typ_id) != ::AllData::all_type_setattro.end()) {
        ::AllData::all_type_setattro.erase(typ_id);
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
    clear_obj(typ_id);
}

static void
PrivateAttrType_del(PyObject* cls) noexcept
{
    PrivateAttrType_finalize(cls);
    PyType_Type.tp_dealloc(cls);
}

static const char* PrivateAttrBase_doc = "The class to help to create private attribute. It does not have any special behavior.";

// PrivateAttrBase
static PyObject*
create_private_attr_base_simple(void) noexcept
{
    PyObject* name = PyUnicode_InternFromString("PrivateAttrBase");
    if (!name) return NULL;
    PyObject* bases = PyTuple_New(0);
    if (!bases) {
        Py_DECREF(name);
        return NULL;
    }
    PyObject* dict = PyDict_New();
    if (!dict) {
        Py_DECREF(name);
        Py_DECREF(bases);
        return NULL;
    }
    PyObject *private_attrs = PyTuple_New(0);
    if (!private_attrs) {
        Py_DECREF(name);
        Py_DECREF(bases);
        Py_DECREF(dict);
        return NULL;
    }
    PyDict_SetItemString(dict, "__private_attrs__", private_attrs);
    PyDict_SetItemString(dict, "__slots__", private_attrs);
    PyDict_SetItemString(dict, "__doc__", PyUnicode_InternFromString(PrivateAttrBase_doc));
    PyDict_SetItemString(dict, "__module__", PyUnicode_InternFromString("private_attribute"));
    PyObject *args = PyTuple_Pack(3, name, bases, dict);
    PyObject* base_type;
    if (args) {
        // RAII: the first heap type created through tp_new (PrivateAttrBase)
        // carries CPython's pristine subtype_traverse/subtype_clear in its
        // tp_traverse/tp_clear; PrivateAttrType_new captures those pointers
        // while this guard is active.
        PrivateAttrCaptureGuard capture_guard;
        base_type = PrivateAttrType_new((PyTypeObject*)&PrivateAttrType, args, NULL);
        Py_DECREF(args);
    } else {
        Py_DECREF(name);
        Py_DECREF(bases);
        Py_DECREF(dict);
        return NULL;
    }
    Py_DECREF(name);
    Py_DECREF(bases);
    Py_DECREF(dict);
    if (!base_type) {
        return NULL;
    }
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
    Py_TPFLAGS_DEFAULT, //tp_flags
    0, //tp_doc
    0, //tp_traverse
    0, //tp_clear
    0, //tp_richcompare
    0, //tp_weaklistoffset
    0, //tp_iter
    0, //tp_iternext
    0, //tp_methods
    0, //tp_members
    PrivateTempObject_getsets //tp_getset
};

static PyObject*
prepare_for_PrivateAttr(PyObject* /*self*/, PyObject* args, PyObject* kwargs) noexcept
{
    PrivateAttrCreationData* tmp_data = new PrivateAttrCreationData();
    if (!PrivateAttrType_preprocess(args, kwargs, *tmp_data)) {
        tmp_data->clear();
        delete tmp_data;
        return NULL;
    }
    PrivateTempObject* tmp = PyObject_New(PrivateTempObject, &PrivateTempType);
    if (!tmp) {
        tmp_data->clear();
        delete tmp_data;
        return NULL;
    }
    tmp->tmp = tmp_data;
    return (PyObject*)tmp;
}

static PyObject*
postprocess_for_PrivateAttr(PyObject* /*self*/, PyObject* args) noexcept
{
    PyObject* type;
    PyObject* tmp;
    if (!PyArg_ParseTuple(args, "OO", &type, &tmp)) {
        return NULL;
    }
    if (!PyType_Check(type)) {
        PyErr_SetString(PyExc_TypeError, "type must be a type");
        return NULL;
    }
    if (!PyObject_TypeCheck(tmp, &PrivateTempType)) {
        PyErr_SetString(PyExc_TypeError, "tmp must be a private_temp");
        return NULL;
    }
    if (!PrivateAttrType_postprocess(type, *(((PrivateTempObject*)tmp)->tmp))) {
        return NULL;
    }
    ((PrivateTempObject*)tmp)->tmp->clear();
    Py_RETURN_NONE;
}

#define METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(name) do {\
    if (::AllData::all_type_##name.find(id) != ::AllData::all_type_##name.end()) {\
        ::AllData::all_type_##name.erase(id);\
    }\
} while (0)

static PyObject*
metaclass_weakref_callback(PyObject* self, PyObject* /*weakref*/) noexcept
{
    void* pointer = PyCapsule_GetPointer(self, "metaclass_id");
    if (!pointer) {
        Py_RETURN_NONE;
    }
    uintptr_t id = (uintptr_t)pointer;

    // Clean up the class-level AllData owned by this registered metaclass
    // (type_attr_dict / all_type_subclass_attr / object buckets / caches).
    // This replaces the old tp_finalize hook (register_finalize) which has
    // been removed: a weakref callback fires on every death path (refcount and
    // cyclic GC) without making the metaclass "finalizable".
    PrivateAttrType_finalize((PyObject*)id);

    std::unique_lock lock(::AllData::all_register_new_metaclass_mutex);

    auto it = ::AllData::all_register_type_weak_ref.find(id);
    if (it != ::AllData::all_register_type_weak_ref.end()) {
        Py_DECREF(it->second);
        ::AllData::all_register_type_weak_ref.erase(it);
    }
    METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(getattro);
    METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(setattro);
    METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(traverse);
    METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(clear);
    METACLASS_WEAKREF_CALLBACK_REMOVE_ITEM(del);

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
        PyObject* instance = PyWeakref_GET_OBJECT((PyObject*)instanceref);
        if (instance == metaclass) continue;
        if (PyObject_IsInstance(instance, metaclass)) {
            return true;
        }
#else
        PyObject* instance;
        if (PyWeakref_GetRef(instanceref, &instance) == 1) {
            if (instance == metaclass) {
                Py_DECREF(instance);
                continue;
            }
            if (PyObject_IsInstance(instance, metaclass)) {
                Py_DECREF(instance);
                return true;
            }
            Py_DECREF(instance);
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
    if (need_analyse_type(metaclass)) {
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

    PyObject* capsule = PyCapsule_New((void*)metaclass, "metaclass_id", NULL);
    if (!capsule) {
        return NULL;
    }

    PyObject* callback = PyCFunction_New(&weakref_callback_def, capsule);
    if (!callback) {
        Py_DECREF(capsule);
        return NULL;
    }
    PyObject* ref = PyWeakref_NewRef(metaclass, callback);
    if (!ref) {
        Py_DECREF(callback);
        Py_DECREF(capsule);
        return NULL;
    }
    ::AllData::all_register_type_weak_ref[id] = ref;
    PyTypeObject* mt = (PyTypeObject*)metaclass;
    // Save the ORIGINAL slots before wrapping. Always resolve through the
    // get_type_need_tp_* walkers so a delegating default (CPython's
    // subtype_traverse/subtype_clear, which a plain heap metaclass inherits)
    // is never stored as the "original". The walkers skip those defaults and
    // fall back to PyType_Type.tp_* (the real implementation for a type
    // object). If an entry already exists for this id, keep it (idempotent).
    // save and replace tp_getattro
    if (mt->tp_getattro != PrivateAttrType_getattr) {
        if (::AllData::all_type_getattro.find(id) == ::AllData::all_type_getattro.end()) {
            ::AllData::all_type_getattro[id] = get_type_need_tp_getattro(mt);
        }
        mt->tp_getattro = PrivateAttrType_getattr;
    }
    // save and replace tp_setattro
    if (mt->tp_setattro != PrivateAttrType_setattr) {
        if (::AllData::all_type_setattro.find(id) == ::AllData::all_type_setattro.end()) {
            ::AllData::all_type_setattro[id] = get_type_need_tp_setattro(mt);
        }
        mt->tp_setattro = PrivateAttrType_setattr;
    }
    // save and replace tp_traverse
    if (mt->tp_traverse != PrivateAttrType_tp_traverse) {
        if (::AllData::all_type_traverse.find(id) == ::AllData::all_type_traverse.end()) {
            ::AllData::all_type_traverse[id] = get_type_need_tp_traverse(mt);
        }
        mt->tp_traverse = PrivateAttrType_tp_traverse;
    }
    // save and replace tp_clear
    if (mt->tp_clear != PrivateAttrType_tp_clear) {
        if (::AllData::all_type_clear.find(id) == ::AllData::all_type_clear.end()) {
            ::AllData::all_type_clear[id] = get_type_need_tp_clear(mt);
        }
        mt->tp_clear = PrivateAttrType_tp_clear;
    }
    Py_RETURN_NONE;
}

static PyObject*
register_metaclass(PyObject* /*self*/, PyObject* metaclass) noexcept
{
    if (!register_metaclass_head(metaclass)) return NULL;
    PyObject* subclasses = PyObject_CallMethod((PyObject*)&PyType_Type, "__subclasses__", "O", metaclass);
    if (!subclasses) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    Py_ssize_t list_len = PyList_GET_SIZE(subclasses);
    for (Py_ssize_t i = 0; i < list_len; i++) {
        PyObject* subclass = PyList_GET_ITEM(subclasses, i);
        if (!register_metaclass(0, subclass)) {
            Py_DECREF(subclasses);
            return NULL;
        }
    }
    Py_DECREF(subclasses);
    Py_RETURN_NONE;
}

static PyObject*
ensure_type_tp(PyObject* /*self*/, PyObject* type) noexcept
{
    if (!PyType_Check(type)) {
        PyErr_SetString(PyExc_TypeError, "type must be a type");
        return NULL;
    }
    if (!need_analyse_type(type)) {
        Py_RETURN_NONE;
    }
    ensure_tp((PyTypeObject*)type);
    ensure_subclass_tp((PyTypeObject*)type);
    Py_RETURN_NONE;
}

static PyObject*
ensure_metaclass_tp(PyObject* /*self*/, PyObject* metaclass) noexcept
{
    uintptr_t id = (uintptr_t)metaclass;
    if (::AllData::all_register_type_weak_ref.find(id) != ::AllData::all_register_type_weak_ref.end()) {
        PyTypeObject* mt = (PyTypeObject*)metaclass;
        // Same as register_metaclass_head: always resolve the ORIGINAL slots
        // through the get_type_need_tp_* walkers (never store a delegating
        // default such as subtype_traverse/subtype_clear). Idempotent when an
        // entry already exists.
        if (mt->tp_getattro != PrivateAttrType_getattr) {
            if (::AllData::all_type_getattro.find(id) == ::AllData::all_type_getattro.end()) {
                ::AllData::all_type_getattro[id] = get_type_need_tp_getattro(mt);
            }
            mt->tp_getattro = PrivateAttrType_getattr;
        }
        if (mt->tp_setattro != PrivateAttrType_setattr) {
            if (::AllData::all_type_setattro.find(id) == ::AllData::all_type_setattro.end()) {
                ::AllData::all_type_setattro[id] = get_type_need_tp_setattro(mt);
            }
            mt->tp_setattro = PrivateAttrType_setattr;
        }
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
    Py_RETURN_NONE;
}

static PyObject*
PrivateModule_get_PrivateWrapProxy(PyObject* /*self*/, void* /*closure*/) noexcept
{
    PyObject* PythonPrivateWrapProxy = (PyObject*)&PrivateWrapProxyType;
    Py_INCREF(PythonPrivateWrapProxy);
    return PythonPrivateWrapProxy;
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
    static PyObject* PrivateAttrBase = create_private_attr_base_simple();
    if (!PrivateAttrBase) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError, "failed to create PrivateAttrBase");
        }
        return NULL;
    }
    Py_INCREF(PrivateAttrBase);
    return PrivateAttrBase;
}

static int
PyList_AppendString(PyObject* list, const char* str) noexcept
{
    PyObject* obj = PyUnicode_InternFromString(str);
    if (!obj) return -1;
    if (PyList_Append(list, obj) < 0) {
        Py_DECREF(obj);
        return -1;
    }
    Py_DECREF(obj);
    return 0;
}

static PyObject*
PrivateModule_dir(PyObject* self, PyObject* /*args*/) noexcept
{
    PyObject* parent_dir = PyObject_CallMethod((PyObject*)&PyModule_Type, "__dir__", "O", self);
    if (!parent_dir) return NULL;
    PyObject* attr_list = PyList_New(0);
    if (!attr_list) {
        Py_DECREF(parent_dir);
        return NULL;
    }
    PyList_AppendString(attr_list, "PrivateWrapProxy");
    PyList_AppendString(attr_list, "PrivateAttrType");
    PyList_AppendString(attr_list, "PrivateAttrBase");
    PyList_AppendString(attr_list, "prepare");
    PyList_AppendString(attr_list, "postprocess");
    PyList_AppendString(attr_list, "register_metaclass");
    PyList_AppendString(attr_list, "ensure_type");
    PyList_AppendString(attr_list, "ensure_metaclass");
    PyObject* result = PySequence_Concat(parent_dir, attr_list);
    Py_DECREF(parent_dir);
    Py_DECREF(attr_list);
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
            "prepare",
            "postprocess",
            "register_metaclass",
            "ensure_type",
            "ensure_metaclass"
        };
        const size_t num_attrs = sizeof(unsetable_attrs) / sizeof(unsetable_attrs[0]);
        for (size_t i = 0; i < num_attrs; ++i) {
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
        temp = private_attribute.prepare(*args, **kwargs)
        typ = super().__new__(cls, temp.name, temp.bases, temp.attrs, **temp.kwds)
        private_attribute.postprocess(typ, temp)
        return typ

private_attribute.register_metaclass(PrivateAbcMeta)
```
)";

static const char* ensure_type_doc = R"(function for custom metaclass to ensure the type is a private attributes class.
def ensure_type(type: type) -> None:
    the function to ensure the type is a private attributes class `tp_getattro`, `tp_setattro` and `tp_finalizer`.
)";

static const char* ensure_metaclass_doc = R"(function for custom metaclass to ensure the metaclass is working.
def ensure_metaclass(metaclass: type) -> None:
    the function to ensure the metaclass `tp_getattro`, `tp_setattro` and `tp_finalizer`.
)";

static PyMethodDef PrivateModule_methods[] = {
    {"prepare", (PyCFunction)prepare_for_PrivateAttr, METH_VARARGS | METH_KEYWORDS | METH_STATIC, prepare_and_postprocess_doc},
    {"postprocess", (PyCFunction)postprocess_for_PrivateAttr, METH_VARARGS | METH_STATIC, prepare_and_postprocess_doc},
    {"register_metaclass", (PyCFunction)register_metaclass, METH_O | METH_STATIC, prepare_and_postprocess_doc},
    {"ensure_type", (PyCFunction)ensure_type_tp, METH_O | METH_STATIC, ensure_type_doc},
    {"ensure_metaclass", (PyCFunction)ensure_metaclass_tp, METH_O | METH_STATIC, ensure_metaclass_doc},
    {NULL}  // Sentinel
};

static PyObject*
PrivateModule_get_prepare(PyObject* /*self*/, void* /*closure*/) noexcept
{
    return PyCFunction_NewEx(&PrivateModule_methods[0], NULL, NULL);
}

static PyObject*
PrivateModule_get_postprocess(PyObject* /*self*/, void* /*closure*/) noexcept
{
    return PyCFunction_NewEx(&PrivateModule_methods[1], NULL, NULL);
}

static PyObject*
PrivateModule_get_register_metaclass(PyObject* /*self*/, void* /*closure*/) noexcept
{
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

static PyGetSetDef PrivateModule_getsetters[] = {
    {"PrivateWrapProxy", (getter)PrivateModule_get_PrivateWrapProxy, NULL, NULL, NULL},
    {"PrivateAttrType", (getter)PrivateModule_get_PrivateAttrType, NULL, NULL, NULL},
    {"PrivateAttrBase", (getter)PrivateModule_get_PrivateAttrBase, NULL, NULL, NULL},
    {"prepare", (getter)PrivateModule_get_prepare, NULL, NULL, NULL},
    {"postprocess", (getter)PrivateModule_get_postprocess, NULL, NULL, NULL},
    {"register_metaclass", (getter)PrivateModule_get_register_metaclass, NULL, NULL, NULL},
    {"ensure_type", (getter)PrivateModule_get_ensure_type, NULL, NULL, NULL},
    {"ensure_metaclass", (getter)PrivateModule_get_ensure_metaclass, NULL, NULL, NULL},
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
    PrivateModule_GETATTRO_CASE(prepare);
    PrivateModule_GETATTRO_CASE(postprocess);
    PrivateModule_GETATTRO_CASE(register_metaclass);
    PrivateModule_GETATTRO_CASE(ensure_type);
    PrivateModule_GETATTRO_CASE(ensure_metaclass);
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

static PyModuleDef def = {
    PyModuleDef_HEAD_INIT,
    "private_attribute",
    module_doc,
    0,
    NULL,
    NULL,
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

static int
atexit_register_clean_func(void) noexcept
{
    PyObject* atexit = PyImport_ImportModule("atexit");
    if (!atexit) {
        return -1;
    }

    PyObject* func = PyCFunction_New(&clean_storage, NULL);
    if (!func) {
        Py_DECREF(atexit);
        return -1;
    }

    PyObject* result = PyObject_CallMethod(atexit, "register", "O", func);
    Py_DECREF(func);
    Py_DECREF(atexit);

    if (!result) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

PyMODINIT_FUNC
PyInit_private_attribute(void) noexcept
{
    // check if AllData::store_module_self has first value
    if (AllData::store_module_self.size() >= 1) {
        PyObject* m = AllData::store_module_self[0];
        Py_INCREF(m);
        return m;
    }
    if (init_all_slots() <0 ||
        atexit_register_clean_func() < 0 ||
        PyType_Ready(&PrivateWrapType) < 0 ||
        PyType_Ready(&PrivateWrapProxyType) < 0 ||
        PyType_Ready(&PrivateAttrType) < 0 ||
        PyType_Ready(&PrivateModuleType) < 0 ||
        PyType_Ready(&PrivateTempType) < 0) {
        return NULL;
    }
    // Eagerly create PrivateAttrBase so that CPython's subtype_traverse /
    // subtype_clear pointers are captured at import time (see
    // PrivateAttrCaptureGuard and ::AllData::captured_subtype_*), instead of
    // waiting for the first lazy attribute access to the module.
    PyObject* private_attr_base = PrivateModule_get_PrivateAttrBase(NULL, NULL);
    if (!private_attr_base) {
        return NULL;
    }
    PyObject* all = PyList_New(0);
    if (!all) {
        Py_DECREF(private_attr_base);
        return NULL;
    }
    PyObject* m = PyModule_Create(&def);
    if (!m) {
        Py_DECREF(all);
        Py_DECREF(private_attr_base);
        return NULL;
    }
#ifdef Py_GIL_DISABLED
    PyUnstable_Module_SetGIL(m, Py_MOD_GIL_NOT_USED);
#endif
    PyModule_AddObject(m, "__all__", all);
    /* all contains:
     * PrivateWrapProxy
     * PrivateAttrType
     * PrivateAttrBase
     * prepare
     * postprocess
     * register_metaclass
     * ensure_type
     * ensure_metaclass
     */
    PyList_AppendString(all, "PrivateWrapProxy");
    PyList_AppendString(all, "PrivateAttrType");
    PyList_AppendString(all, "PrivateAttrBase");
    PyList_AppendString(all, "prepare");
    PyList_AppendString(all, "postprocess");
    PyList_AppendString(all, "register_metaclass");
    PyList_AppendString(all, "ensure_type");
    PyList_AppendString(all, "ensure_metaclass");
    Py_SET_TYPE(m, &PrivateModuleType);

    Py_DECREF(private_attr_base);
    AllData::store_module_self.push_back(m);    // store module self

    return m;
}
