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
#include <vector>
#include <random>
#include <mutex>
#include <shared_mutex>
#include "picosha2.h"
#include <functional>
#include <memory>
#include <algorithm>

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
};

namespace {
    namespace AllData {
        static std::unordered_map<AllPyobjectAttrCacheKey, std::string> cache;
        static std::unordered_set<std::string> all_exist_name;
        static std::unordered_map<uintptr_t, std::vector<AllPyobjectAttrCacheKey>> obj_attr_keys;
        static std::shared_mutex cache_mutex;
        namespace {
            static std::unordered_map<uintptr_t, std::unordered_map<std::string, PyObject*>> type_attr_dict;
        };
        static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t, PyCodeObject*>> type_allowed_code_map;
        static std::unordered_map<uintptr_t, std::shared_ptr<std::shared_mutex>> all_type_mutex;
        static std::unordered_map<uintptr_t, PyObject*> type_need_call;
        static std::unordered_map<uintptr_t, std::unordered_set<TwoStringTuple>> all_type_attr_set;
        namespace {
            static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t,
            std::unordered_map<std::string, PyObject*>>> all_object_attr, all_type_subclass_attr;
        };
        static std::unordered_map<uintptr_t, std::unordered_map<uintptr_t, std::shared_ptr<std::shared_mutex>>>
        all_object_mutex, all_type_subclass_mutex;
        static std::unordered_map<uintptr_t, std::vector<uintptr_t>> all_type_parent_id;
        // all type tp_getattro map
        static std::unordered_map<uintptr_t, getattrofunc> all_type_getattro;
        // all type tp_setattro map
        static std::unordered_map<uintptr_t, setattrofunc> all_type_setattro;
        // all type tp_finalizer map
        static std::unordered_map<uintptr_t, destructor> all_type_finalize;

        static std::shared_mutex all_register_new_metaclass_mutex;
        static std::unordered_map<uintptr_t, PyObject*> all_register_type_weak_ref;
    };
};

namespace AllSlots {
    static getattrofunc original_getattro = nullptr;
    static setattrofunc original_setattro = nullptr;
    static destructor original_finalize = nullptr;
}

struct FinalObject
{
    PyObject* result = NULL;
    int status = 0;
    FinalObject(PyObject* result) noexcept
        : result(result) {
            Py_XINCREF(result);
        }
    FinalObject(int status) noexcept: status(status) {}
    ~FinalObject() noexcept {
        Py_XDECREF(result);
    }
};

static TwoStringTuple get_string_hash_tuple2(const std::string& name) noexcept;
static PyCodeObject* get_now_code() noexcept;
static uintptr_t type_set_attr_long_long_guidance(uintptr_t type, const std::string& name) noexcept;
static bool type_private_attr(uintptr_t type, const std::string& name) noexcept;
static FinalObject type_get_final_attr(uintptr_t type_id, const std::string& name) noexcept;

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

static bool
attr_classify(uintptr_t typ_id, const std::string& name, PyCodeObject* code) noexcept
{
    if (is_class_code(typ_id, code)) {
        return true;
    }
    if (is_type_private(typ_id, name)) {
        return false;
    }
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()) {
        std::vector<uintptr_t> parent_ids = ::AllData::all_type_parent_id[typ_id];
        for (auto& parent_id : parent_ids) {
            if (is_class_code(parent_id, code)) {
                return true;
            }
            if (is_type_private(parent_id, name)) {
                return false;
            }
        }
    }
    return true;
}

static std::string
generate_private_attr_name(uintptr_t obj_id, const std::string& attr_name) noexcept
{
    std::string combined = std::to_string(obj_id) + "_" + compile_time + "_" + attr_name;
    std::string hash_str = picosha2::hash256_hex_string(combined);

    unsigned long long seed = std::stoul(hash_str.substr(0, 8), nullptr, 16);

    std::mt19937 rng(seed);

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

class RestorePythonException : public std::exception
{
public:
    RestorePythonException(PyObject* type, PyObject* value, PyObject* traceback) noexcept
        : type(type), value(value), traceback(traceback) {
    }

    ~RestorePythonException() noexcept {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
    }

    RestorePythonException(const RestorePythonException&) = delete;
    RestorePythonException& operator=(RestorePythonException&& other) noexcept {
        if (this != &other) {
            type = other.type;
            value = other.value;
            traceback = other.traceback;
            other.type = nullptr;
            other.value = nullptr;
            other.traceback = nullptr;
        }
        return *this;
    }

    // Move constructor
    RestorePythonException(RestorePythonException&& other) noexcept
        : type(other.type), value(other.value), traceback(other.traceback) {
        other.type = nullptr;
        other.value = nullptr;
        other.traceback = nullptr;
    }

    void restore() noexcept {
        PyErr_Restore(type, value, traceback);
        type = value = traceback = nullptr;
    }

private:
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
};

static std::string
custom_random_string(uintptr_t obj_id, const std::string& attr_name, PyObject* func)
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
            PyObject* args = PyTuple_New(2);
            PyTuple_SetItem(args, 0, PyLong_FromSize_t(static_cast<size_t>(obj_id)));
            PyTuple_SetItem(args, 1, PyUnicode_FromString(attr_name.c_str()));

            PyObject* python_result = PyObject_CallObject((PyObject*)func, args);

            Py_DECREF(args);
            if (python_result) {
                if (!PyUnicode_Check(python_result)) {
                    Py_DECREF(python_result);
                    PyErr_SetString(PyExc_TypeError, "private_func function must return a string");
                    PyObject *type, *value, *traceback;
                    PyErr_Fetch(&type, &value, &traceback);
                    throw RestorePythonException(type, value, traceback);
                }
                result = PyUnicode_AsUTF8(python_result);
                Py_DECREF(python_result);
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
            } else {
                PyObject *type, *value, *traceback;
                PyErr_Fetch(&type, &value, &traceback);
                throw RestorePythonException(type, value, traceback);
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

static PyObject*
id_getattr(const std::string& attr_name, PyObject* obj, PyObject* typ) noexcept
{
    uintptr_t obj_id, typ_id, final_id;
    obj_id = (uintptr_t) obj;
    typ_id = (uintptr_t) typ;
    final_id = type_set_attr_long_long_guidance(typ_id, attr_name);
    FinalObject final_object = type_get_final_attr(typ_id, attr_name);
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
        try {
            obj_private_name = custom_random_string(obj_id, attr_name, obj_need_call);
        } catch (RestorePythonException& e) {
            e.restore();
            return NULL;
        }
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
    }

    if (::AllData::all_object_attr.find(final_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return NULL;
    }
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
    PyObject* result = NULL;
    if (final_object.status != -1) {
        result = final_object.result;
    }
    PyTypeObject* result_typ = nullptr;

    if (result) {
        result_typ = Py_TYPE(result);
        if (!result_typ) return NULL;
    }
    bool has_get = false;
    bool has_set = false;
    if (result_typ) {
        has_get = result_typ->tp_descr_get != NULL;
        has_set = result_typ->tp_descr_set != NULL;
    }
    if (has_get && has_set) {
        PyObject* final_result = result_typ->tp_descr_get(result, obj, typ);
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
                if (obj_type->tp_descr_get != NULL) {
                    lock.unlock();
                    return obj_type->tp_descr_get(python_obj, Py_None, obj);
                }
            }
            Py_XINCREF(python_obj);
            return python_obj;
        }
    }

    if (has_get) {
        PyObject* final_result = result_typ->tp_descr_get(result, obj, typ);
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
    std::string string_type_name = type_name;
    std::string exception_information = "'" + string_type_name + "' objects has no attribute '" + attr_name + "'";
    PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
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
        if (type->tp_descr_get != NULL) {
            PyObject* final_result = type->tp_descr_get(result, Py_None, (PyObject*)type);
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
    std::string string_type_name = type_name;
    std::string message = "type object '" + string_type_name + "' has no attribute '" + attr_name + "'";
    PyErr_SetString(PyExc_AttributeError, message.c_str());
    return NULL;
}

static int
id_setattr(const std::string& attr_name, PyObject* obj, PyObject* typ, PyObject* value) noexcept
{
    uintptr_t obj_id, typ_id, final_id;
    obj_id = (uintptr_t) obj;
    typ_id = (uintptr_t) typ;
    final_id = type_set_attr_long_long_guidance(typ_id, attr_name);
    FinalObject final_object = type_get_final_attr(typ_id, attr_name);
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
        try {
            obj_private_name = custom_random_string(obj_id, attr_name, obj_need_call);
        } catch (RestorePythonException& e) {
            e.restore();
            return -1;
        }
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
    }

    if (::AllData::all_object_attr.find(final_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return -1;
    }
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
    // first: call __set__ method
    PyObject* result = NULL;
    if (final_object.status != -1) {
        result = final_object.result;
    }
    if (result) {
        PyTypeObject* type = Py_TYPE(result);
        if (type->tp_descr_set != NULL) {
            if (type->tp_descr_set(result, obj, value) < 0) {
                return -1;
            }
            return 0;
        }
    }

    // second: set attribute on obj
    Py_INCREF(value);
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[final_id][obj_id]);
        if (::AllData::all_object_attr[final_id][obj_id].find(obj_private_name) != ::AllData::all_object_attr[final_id][obj_id].end()) {
            Py_XDECREF(::AllData::all_object_attr[final_id][obj_id][obj_private_name]);
        }
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
        try {
            final_key = custom_random_string(typ_id, attr_name, type_need_call);
        } catch (RestorePythonException& e) {
            e.restore();
            return -1;
        }
    } else {
        final_key = default_random_string(typ_id, attr_name);
    }
    if (final_id == 0) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return -1;
    }
    if (final_id == typ_id) {
        if (::AllData::type_attr_dict.find(typ_id) == ::AllData::type_attr_dict.end()) {
            ::AllData::type_attr_dict[typ_id] = {};
        }
        if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
            std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
            ::AllData::all_type_mutex[typ_id] = lock;
        }
        {
            std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
            if (::AllData::type_attr_dict[typ_id].find(final_key) != ::AllData::type_attr_dict[typ_id].end()) {
                Py_XDECREF(::AllData::type_attr_dict[typ_id][final_key]);
            }
            ::AllData::type_attr_dict[typ_id][final_key] = value;
            Py_INCREF(value);
        }
        return 0;
    } else {
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
        {
            std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[final_id][typ_id]);
            if (::AllData::all_type_subclass_attr[final_id][typ_id].find(final_key) != ::AllData::all_type_subclass_attr[final_id][typ_id].end()) {
                Py_XDECREF(::AllData::all_type_subclass_attr[final_id][typ_id][final_key]);
            }
            ::AllData::all_type_subclass_attr[final_id][typ_id][final_key] = value;
            Py_INCREF(value);
            return 0;
        }
    }
}

static int
id_delattr(const std::string& attr_name, PyObject* obj, PyObject* typ) noexcept
{
    uintptr_t obj_id, typ_id, final_id;
    obj_id = (uintptr_t) obj;
    typ_id = (uintptr_t) typ;
    final_id = type_set_attr_long_long_guidance(typ_id, attr_name);
    FinalObject final_object = type_get_final_attr(typ_id, attr_name);
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
        try {
            obj_private_name = custom_random_string(obj_id, attr_name, obj_need_call);
        } catch (RestorePythonException& e) {
            e.restore();
            return -1;
        }
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
    }

    if (::AllData::all_object_attr.find(final_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return -1;
    }
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
    // first: find attribute on type to find "__delete__"
    PyObject* result = NULL;
    if (final_object.status == 0) {
        result = final_object.result;
    }
    if (result) {
        PyTypeObject* type = Py_TYPE(result);
        if (type->tp_descr_set != NULL) {
            if (type->tp_descr_set(result, result, NULL) < 0) {
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
            std::string string_type_name = type_name;
            std::string exception_information = "'" + string_type_name + "' objects has no attribute '" + attr_name + "'";
            PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
            return -1;
        }
        PyObject* delete_obj = ::AllData::all_object_attr[final_id][obj_id][obj_private_name];
        ::AllData::all_object_attr[final_id][obj_id].erase(obj_private_name);
        Py_XDECREF(delete_obj);
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
        try {
            final_key = custom_random_string(typ_id, attr_name, type_need_call);
        } catch (RestorePythonException& e) {
            e.restore();
            return -1;
        }
    } else {
        final_key = default_random_string(typ_id, attr_name);
    }
    if (final_id == 0) {
        PyErr_SetString(PyExc_TypeError, "type not found");
        return -1;
    }
    if (typ_id == final_id) {
        if (::AllData::type_attr_dict.find(typ_id) == ::AllData::type_attr_dict.end()) {
            ::AllData::type_attr_dict[typ_id] = {};
        }
        if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
            std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
            ::AllData::all_type_mutex[typ_id] = lock;
        }
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
        if (::AllData::type_attr_dict[typ_id].find(final_key) == ::AllData::type_attr_dict[typ_id].end()) {
            const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
            if (type_name == NULL) {
                return -1;
            }
            std::string string_type_name = type_name;
            std::string message = "type object '" + string_type_name + "' has no attribute '" + attr_name + "'";
            PyErr_SetString(PyExc_AttributeError, message.c_str());
            return -1;
        }
        PyObject* delete_obj = ::AllData::type_attr_dict[typ_id][final_key];
        ::AllData::type_attr_dict[typ_id].erase(final_key);
        Py_XDECREF(delete_obj);
    } else {
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
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_subclass_mutex[final_id][typ_id]);
        if (::AllData::all_type_subclass_attr[final_id][typ_id].find(final_key) == ::AllData::all_type_subclass_attr[final_id][typ_id].end()) {
            const char* type_name = get_name_from_tp_name((PyTypeObject*)typ);
            if (type_name == NULL) {
                return -1;
            }
            std::string string_type_name = type_name;
            std::string message = "type object '" + string_type_name + "' has no attribute '" + attr_name + "'";
            PyErr_SetString(PyExc_AttributeError, message.c_str());
            return -1;
        }
        PyObject* delete_obj = ::AllData::all_type_subclass_attr[final_id][typ_id][final_key];
        ::AllData::all_type_subclass_attr[final_id][typ_id].erase(final_key);
        Py_XDECREF(delete_obj);
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
        return PyUnicode_FromString("PrivateWrap");
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
        return PyUnicode_FromString("private_attribute");
    }
    PyObject* module = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__module__");
    if (!module){
        PyErr_Clear();
        return PyUnicode_FromString("private_attribute");
    }
    return module;
}

static PyObject*
PrivateWarp_name(PyObject* obj, void* /*closure*/) noexcept
{
    if (!obj) {
        return PyUnicode_FromString("_PrivateWrap");
    }
    PyObject* name = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__name__");
    if (!name) {
        PyErr_Clear();
        return PyUnicode_FromString("_PrivateWrap");
    }
    return name;
}

static PyObject*
PrivateWrap_qualname(PyObject* obj, void* /*closure*/) noexcept
{
    if (!obj) {
        return PyUnicode_FromString("_PrivateWrap");
    }
    PyObject* qualname = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__qualname__");
    if (!qualname) {
        PyErr_Clear();
        return PyUnicode_FromString("_PrivateWrap");
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

static const char* PrivateWrap_result_doc = "the final result of decorating";
static const char* PrivateWrap_funcs_doc = "the original functions";

static PyGetSetDef PrivateWrap_getset[] = {
    {"funcs", (getter)PrivateWrap_funcs, NULL, PrivateWrap_funcs_doc, NULL},
    {"__doc__", (getter)PrivateWrap_doc, NULL, NULL, NULL},
    {"__module__", (getter)PrivateWrap_module, NULL, NULL, NULL},
    {"__name__", (getter)PrivateWarp_name, NULL, NULL, NULL},
    {"__qualname__", (getter)PrivateWrap_qualname, NULL, NULL, NULL},
    {"__annotate__", (getter)PrivateWrap_annotate, NULL, NULL, NULL},
    {"__type_params__", (getter)PrivateWrap_type_params, NULL, NULL, NULL},
    {NULL}
};

static PyObject *
PrivateWrap_getattro(PyObject *obj, PyObject *name) noexcept
{
    PyObject *res = PyObject_GenericGetAttr(obj, name);
    if (res != NULL) {
        return res;
    }

    PyErr_Clear();

    PrivateWrapObject *self = (PrivateWrapObject *)obj;
    return PyObject_GetAttr(self->result, name);
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
    "_PrivateWrap",                    // tp_doc
    0,                                 // tp_traverse
    0,                                 // tp_clear
    0,                                 // tp_richcompare
    0,                                 // tp_weaklistoffset
    0,                                 // tp_iter
    0,                                 // tp_iternext
    0,                                 // tp_methods
    PrivateWrap_members,               // tp_members
    PrivateWrap_getset,                // tp_getset
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

static PyObject*
PrivateAttr_tp_getattro(PyObject* self, PyObject* name) noexcept
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t type_id = (uintptr_t)typ;
    PrivateAttr_object_init_private_dict((uintptr_t)self, type_id);
    std::string name_str = PyUnicode_AsUTF8(name);
    auto code = get_now_code();
    if (type_private_attr(type_id, name_str)) {
        if (!code || !attr_classify(type_id, name_str, code)){
            Py_XDECREF(code);
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            return NULL;
        } else {
            Py_XDECREF(code);
            return id_getattr(name_str, self, (PyObject*)typ);
        }
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
        if (!code || !attr_classify(typ_id, name_str, code)){
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            Py_XDECREF(code);
            return -1;
        } else {
            Py_XDECREF(code);
            if (!value) {
                return id_delattr(name_str, self, (PyObject*)typ);
            }
            return id_setattr(name_str, self, (PyObject*)typ, value);
        }
    }
    Py_XDECREF(code);
    if (::AllData::all_type_setattro.find(typ_id) != ::AllData::all_type_setattro.end()){
        int result = ::AllData::all_type_setattro[typ_id](self, name, value);
        ensure_tp(typ);
        return result;
    }
    return PyObject_GenericSetAttr(self, name, value);
}

static void
PrivateAttr_tp_finalize(PyObject* self) noexcept
{
    uintptr_t id_self = (uintptr_t)self;
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t typ_id = (uintptr_t)typ;
    Py_ssize_t original_ref = Py_REFCNT(self);
    if (::AllData::all_type_finalize.find(typ_id) != ::AllData::all_type_finalize.end()){
        if (::AllData::all_type_finalize[typ_id]) {
            ::AllData::all_type_finalize[typ_id](self);
            ensure_tp(typ);
        }
    }
    if (original_ref != Py_REFCNT(self)) {
        return;
    }
    std::vector<uintptr_t> parent_ids;
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()){
        parent_ids = ::AllData::all_type_parent_id[typ_id];
    }

    {
        // first: clear ::AllData::all_object_attr and ::AllData::all_object_mutex on this typ_id
        if (::AllData::all_object_attr.find(typ_id) != ::AllData::all_object_attr.end()){
            auto& all_object_attr = ::AllData::all_object_attr[typ_id];
            if (all_object_attr.find(id_self) != all_object_attr.end()){
                auto& all_object_attr_self = all_object_attr[id_self];
                for (auto& attr : all_object_attr_self){
                    Py_XDECREF(attr.second);
                }
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
                    auto& all_object_attr_self = all_object_attr[id_self];
                    for (auto& attr : all_object_attr_self){
                        Py_XDECREF(attr.second);
                    }
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
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   // tp_flags
    "metaclass for private attributes",         // tp_doc
    0,                                          // tp_travers
    0,                                          // tp_clear
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
    TwoStringTuple hash_tuple = get_string_hash_tuple2(name);
    if (::AllData::all_type_attr_set.find(type_id) != ::AllData::all_type_attr_set.end()) {
        if (::AllData::all_type_attr_set[type_id].find(hash_tuple) != ::AllData::all_type_attr_set[type_id].end()) {
            PyObject* type_need_call = NULL;
            if (::AllData::type_need_call.find(type_id) != ::AllData::type_need_call.end()) {
                type_need_call = ::AllData::type_need_call[type_id];
            }
            std::string key;
            if (type_need_call != NULL) {
                try {
                    key = custom_random_string(type_id, name, type_need_call);
                } catch (RestorePythonException& e) {
                    e.restore();
                    return -2; // -2 means exception
                }
            } else {
                key = default_random_string(type_id, name);
            }
            if (::AllData::all_type_mutex.find(type_id) == ::AllData::all_type_mutex.end()) {
                std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
                ::AllData::all_type_mutex[type_id] = lock;
            }
            if (::AllData::type_attr_dict.find(type_id) == ::AllData::type_attr_dict.end()) {
                ::AllData::type_attr_dict[type_id] = {};
            }
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
                                        try {
                                            key = custom_random_string(now_visited_id, name, func);
                                        } catch (RestorePythonException& e) {
                                            e.restore();
                                            return -2; // -2 means exception
                                        }
                                    } else {
                                        key = default_random_string(now_visited_id, name);
                                    }
                                } else {
                                    key = default_random_string(now_visited_id, name);
                                }
                                if (::AllData::all_type_subclass_mutex.find(parent_id) == ::AllData::all_type_subclass_mutex.end()) {
                                    ::AllData::all_type_subclass_mutex[parent_id] = {};
                                }
                                if (::AllData::all_type_subclass_mutex[parent_id].find(now_visited_id) == ::AllData::all_type_subclass_mutex[parent_id].end()) {
                                    std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
                                    ::AllData::all_type_subclass_mutex[parent_id][now_visited_id] = lock;
                                }
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
                            try {
                                key = custom_random_string(parent_id, name, ::AllData::type_need_call[parent_id]);
                            } catch (RestorePythonException& e) {
                                e.restore();
                                return -2; // -2 means exception
                            }
                        } else {
                            key = default_random_string(parent_id, name);
                        }
                    } else {
                        key = default_random_string(parent_id, name);
                    }
                    if (::AllData::all_type_mutex.find(parent_id) == ::AllData::all_type_mutex.end()) {
                        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
                        ::AllData::all_type_mutex[parent_id] = lock;
                    }
                    if (::AllData::type_attr_dict.find(parent_id) != ::AllData::type_attr_dict.end()) {
                        auto& item_set = ::AllData::type_attr_dict[parent_id];
                        std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[parent_id]);
                        if (item_set.find(key) != item_set.end()) {
                            PyObject* obj = item_set[key];
                            return obj;
                        }
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

static void
analyse_all_code(PyObject* obj, std::unordered_map<uintptr_t, PyCodeObject*>& map, std::unordered_set<uintptr_t>& _seen) noexcept
{
    uintptr_t obj_id = (uintptr_t)obj;
    if (_seen.find(obj_id) != _seen.end()) {
        return;
    }
    _seen.insert(obj_id);
    if (PyObject_TypeCheck(obj, &PyCode_Type)) {
        Py_INCREF(obj);
        map[(uintptr_t)obj] = (PyCodeObject*)obj;
        PyObject* co_contain = PyObject_GetAttrString(obj, "co_consts");
        if (co_contain && PySequence_Check(co_contain)) {
            Py_ssize_t len = PySequence_Length(co_contain);
            for (Py_ssize_t i = 0; i < len; i++) {
                PyObject* item = PySequence_GetItem(co_contain, i);
                if (item) {
                    analyse_all_code(item, map, _seen);
                } else {
                    PyErr_Clear();
                }
            }
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PrivateWrapType)) {
        PyObject* func_list = ((PrivateWrapObject*)obj)->func_list;
        if (func_list && PySequence_Check(func_list)) {
            Py_ssize_t len = PySequence_Length(func_list);
            for (Py_ssize_t i = 0; i < len; i++) {
                PyObject* func = PySequence_GetItem(func_list, i);
                if (func) {
                    analyse_all_code(func, map, _seen);
                } else {
                    PyErr_Clear();
                }
            }
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PyProperty_Type)) {
        PyObject* fget = PyObject_GetAttrString(obj, "fget");
        if (fget) {
            analyse_all_code(fget, map, _seen);
        } else {
            PyErr_Clear();
        }
        PyObject* fset = PyObject_GetAttrString(obj, "fset");
        if (fset) {
            analyse_all_code(fset, map, _seen);
        }
        else {
            PyErr_Clear();
        }
        PyObject* fdel = PyObject_GetAttrString(obj, "fdel");
        if (fdel) {
            analyse_all_code(fdel, map, _seen);
        } else {
            PyErr_Clear();
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PyClassMethod_Type) || PyObject_TypeCheck(obj, &PyStaticMethod_Type)) {
        PyObject* func = PyObject_GetAttrString(obj, "__func__");
        if (func) {
            analyse_all_code(func, map, _seen);
        } else {
            PyErr_Clear();
        }
        return;
    }
    PyObject* wrap = PyObject_GetAttrString(obj, "__wrapped__");
    if (wrap) {
        if (wrap == obj) {
            PyObject* code = PyObject_GetAttrString(obj, "__code__");
            if (code) {
                analyse_all_code(code, map, _seen);
            } else {
                PyErr_Clear();
            }
            return;
        }
        analyse_all_code(wrap, map, _seen);
        return;
    }
    else {
        PyErr_Clear();
    }
    PyObject* code = PyObject_GetAttrString(obj, "__code__");
    if (code) {
        analyse_all_code(code, map, _seen);
    } else {
        PyErr_Clear();
    }
}

static std::string
real_class_name(const std::string& name, const std::string& class_name) noexcept
{
    // if the name starts with "__" but does not end with "__", change to _ClassName__name
    if (name.length() >= 2 && name.substr(0, 2) == "__" && name.substr(name.length() - 2) != "__") {
        return "_" + class_name + name;
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
    std::unordered_map<std::string, PyObject*> need_remove_itself;
    std::unordered_map<uintptr_t, std::unordered_map<std::string, PyObject*>> need_remove_subclass;
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

        for (auto& pair : need_remove_itself) {
            Py_XDECREF(pair.second);
        }
        need_remove_itself.clear();

        for (auto& outer_pair : need_remove_subclass) {
            for (auto& inner_pair : outer_pair.second) {
                Py_XDECREF(inner_pair.second);
            }
        }
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
        if (!PyWeakref_CheckRef(metaclassref)) {
            continue;
        }
#if PY_VERSION_HEX < 0x030D0000
        PyObject* metaclass = PyWeakref_GET_OBJECT(metaclassref);
        if (PyObject_IsInstance(type, metaclass)) {
            return true;
        }
#else
        PyObject* metaclass;
        if (PyWeakref_GetRef(metaclassref, &metaclass) == 1) {
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

static void
get_getattribute_and_getattr(PyTypeObject* cls, PyObject** getattribute, PyObject** getattr) noexcept
{
    bool has_getattribute = false;
    bool has_getattr = false;
    if (PyDict_ContainsString(cls->tp_dict, "__getattribute__")) {
        *getattribute = PyDict_GetItemString(cls->tp_dict, "__getattribute__");
        has_getattribute = true;
    }
    if (PyDict_ContainsString(cls->tp_dict, "__getattr__")) {
        *getattr = PyDict_GetItemString(cls->tp_dict, "__getattr__");
        has_getattr = true;
    }
    if (has_getattribute && has_getattr) {
        return;
    }
    for (PyTypeObject* base = cls->tp_base; base != NULL && base != &PyBaseObject_Type; base = base->tp_base) {
        if (!has_getattribute && PyDict_ContainsString(base->tp_dict, "__getattribute__")) {
            *getattribute = PyDict_GetItemString(base->tp_dict, "__getattribute__");
            has_getattribute = true;
        }
        if (!has_getattr && PyDict_ContainsString(base->tp_dict, "__getattr__")) {
            *getattr = PyDict_GetItemString(base->tp_dict, "__getattr__");
            has_getattr = true;
        }
        if (has_getattribute && has_getattr) {
            return;
        }
    }
}

static void
get_setattr_and_delattr(PyTypeObject* cls, PyObject** setattr, PyObject** delattr) noexcept
{
    bool has_setattr = false;
    bool has_delattr = false;
    if (PyDict_ContainsString(cls->tp_dict, "__setattr__")) {
        *setattr = PyDict_GetItemString(cls->tp_dict, "__setattr__");
        has_setattr = true;
    }
    if (PyDict_ContainsString(cls->tp_dict, "__delattr__")) {
        *delattr = PyDict_GetItemString(cls->tp_dict, "__delattr__");
        has_delattr = true;
    }
    if (has_setattr && has_delattr) {
        return;
    }
    for (PyTypeObject* base = cls->tp_base; base != NULL && base != &PyBaseObject_Type; base = base->tp_base) {
        if (!has_setattr && PyDict_ContainsString(base->tp_dict, "__setattr__")) {
            *setattr = PyDict_GetItemString(base->tp_dict, "__setattr__");
            has_setattr = true;
        }
        if (!has_delattr && PyDict_ContainsString(base->tp_dict, "__delattr__")) {
            *delattr = PyDict_GetItemString(base->tp_dict, "__delattr__");
            has_delattr = true;
        }
        if (has_setattr && has_delattr) {
            return;
        }
    }
}

static void
get_del(PyTypeObject* cls, PyObject** del) noexcept
{
    if (PyDict_ContainsString(cls->tp_dict, "__del__")) {
        *del = PyDict_GetItemString(cls->tp_dict, "__del__");
        return;
    }
    for (PyTypeObject* base = cls->tp_base; base != NULL && base != &PyBaseObject_Type; base = base->tp_base) {
        if (PyDict_ContainsString(base->tp_dict, "__del__")) {
            *del = PyDict_GetItemString(base->tp_dict, "__del__");
            return;
        }
    }
}

static PyObject*
python_original_tp_getattro(PyObject* self, PyObject* name) noexcept
{
    PyObject* getattriute = NULL;
    PyObject* getattr = NULL;
    get_getattribute_and_getattr((PyTypeObject*)self->ob_type, &getattriute, &getattr);
    PyObject* res = NULL;
    if (getattriute) {
        res = PyObject_CallFunctionObjArgs(getattriute, self, name, NULL);
    } else {
        res = PyObject_GenericGetAttr(self, name);
    }
    if (!res && getattr) {
        // check if the exception is AttributeError
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            return NULL;
        }
        PyErr_Clear();
        res = PyObject_CallFunctionObjArgs(getattr, self, name, NULL);
    }
    return res;
}

static int
python_original_tp_setattro(PyObject* self, PyObject* name, PyObject* value) noexcept
{
    PyObject* setattr = NULL;
    PyObject* delattr = NULL;
    get_setattr_and_delattr((PyTypeObject*)self->ob_type, &setattr, &delattr);
    if (value && setattr) {
        PyObject* res = PyObject_CallFunctionObjArgs(setattr, self, name, value, NULL);
        if (!res) {
            return -1;
        }
        Py_DECREF(res);
        return 0;
    } else if (!value && delattr) {
        PyObject* res = PyObject_CallFunctionObjArgs(delattr, self, name, NULL);
        if (!res) {
            return -1;
        }
        Py_DECREF(res);
        return 0;
    }
    return PyObject_GenericSetAttr(self, name, value);
}

static void
python_original_tp_finalize(PyObject* self) noexcept
{
    PyObject* del = NULL;
    get_del((PyTypeObject*)self->ob_type, &del);
    if (del) {
        PyObject *exc_type = NULL, *exc_value = NULL, *exc_tb = NULL;
        PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
        PyObject *result = PyObject_CallFunctionObjArgs(del, self, NULL);
        if (result == NULL)  {
            // python < 3.13 use PyErr_WriteUnraisable, python >= 3.13 use PyErr_FormatUnraisable
# if PY_VERSION_HEX >= 0x030D0000
            PyErr_FormatUnraisable("Exception ignored while "
                                   "calling deallocator %R", del);
# else
            PyErr_WriteUnraisable(del);
# endif
        }
        else {
            Py_DECREF(result);
        }
        if (exc_type != NULL) {
            PyErr_Restore(exc_type, exc_value, exc_tb);
        }
    }
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

    if (!PyDict_Check(data.attrs)) {
        PyErr_SetString(PyExc_TypeError, "attrs must be a dict");
        return false;
    }

    PyObject* __private_attrs__ = PyDict_GetItemString(data.attrs, "__private_attrs__");
    if (!__private_attrs__) {
        PyErr_SetString(PyExc_TypeError, "'__private_attrs__' is needed for type 'PrivateAttrType'");
        return false;
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
                std::string error_msg = "invalid attribute name: '" + std::string(*p) + "'";
                PyErr_SetString(PyExc_TypeError, error_msg.c_str());
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
        PyObject* slot_seq = PySequence_Fast(all_slots, "__slots__ must be a sequence");
        if (!slot_seq) {
            return false;
        }

        Py_ssize_t slot_len = PySequence_Fast_GET_SIZE(slot_seq);

        for (Py_ssize_t j = 0; j < slot_len; j++) {
            PyObject* slot = PySequence_Fast_GET_ITEM(slot_seq, j);
            if (PyUnicode_Check(slot)) {
                const char* slot_cstr = PyUnicode_AsUTF8(slot);
                if (data.private_attrs_vector_string.find((std::string)slot_cstr) != data.private_attrs_vector_string.end()){
                    std::string error_msg = "'__slots__' and '__private_attrs__' cannot have the same attribute name: '" + std::string(slot_cstr) + "'";
                    PyErr_SetString(PyExc_TypeError, error_msg.c_str());
                    Py_DECREF(slot_seq);
                    return false;
                }
            }
        }
        Py_DECREF(slot_seq);
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
                Py_INCREF(need_value);
                data.need_remove_itself[attr_name] = need_value;
                PyDict_DelItem(data.attrs_copy, key);
                continue;
            }
            uintptr_t need_remove_subclass_id = need_remove_to_subclass(attr_name);
            if (need_remove_subclass_id) {
                if (data.need_remove_subclass.find(need_remove_subclass_id) == data.need_remove_subclass.end()) {
                    data.need_remove_subclass[need_remove_subclass_id] = {};
                }
                Py_INCREF(need_value);
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
static destructor get_need_tp_finalize(PyTypeObject* cls) noexcept;

static void
ensure_tp(PyTypeObject* type_instance) noexcept
{
    uintptr_t type_id = (uintptr_t)(type_instance);
    {
        if (type_instance->tp_getattro != PrivateAttr_tp_getattro) {
            if (!type_instance->tp_getattro) {
                ::AllData::all_type_getattro[type_id] = get_need_tp_getattro(type_instance);
            } else {
                ::AllData::all_type_getattro[type_id] = type_instance->tp_getattro;
            }
            type_instance->tp_getattro = PrivateAttr_tp_getattro;
        } else {
            if (PyDict_ContainsString(type_instance->tp_dict, "__getattribute__") || PyDict_ContainsString(type_instance->tp_dict, "__getattr__")) {
                ::AllData::all_type_getattro[type_id] = AllSlots::original_getattro;
            } else {
                PyTypeObject* base = type_instance->tp_base;
                uintptr_t base_id = (uintptr_t)(base);
                if (::AllData::all_type_getattro.find(base_id) != ::AllData::all_type_getattro.end()) {
                    ::AllData::all_type_getattro[type_id] = ::AllData::all_type_getattro[base_id];
                } else if (base && base->tp_getattro && base->tp_getattro != PrivateAttr_tp_getattro) {
                    ::AllData::all_type_getattro[type_id] = base->tp_getattro;
                }
            }
        }
    }
    {
        if (type_instance->tp_setattro != PrivateAttr_tp_setattro) {
            if (!type_instance->tp_setattro) {
                ::AllData::all_type_setattro[type_id] = get_need_tp_setattro(type_instance);
            } else {
                ::AllData::all_type_setattro[type_id] = type_instance->tp_setattro;
            }
            type_instance->tp_setattro = PrivateAttr_tp_setattro;
        } else {
            if (PyDict_ContainsString(type_instance->tp_dict, "__setattr__") || PyDict_ContainsString(type_instance->tp_dict, "__delattr__")) {
                ::AllData::all_type_setattro[type_id] = AllSlots::original_setattro;
            } else {
                PyTypeObject* base = type_instance->tp_base;
                uintptr_t base_id = (uintptr_t)(base);
                if (::AllData::all_type_setattro.find(base_id) != ::AllData::all_type_setattro.end()) {
                    ::AllData::all_type_setattro[type_id] = ::AllData::all_type_setattro[base_id];
                } else if (base && base->tp_setattro && base->tp_setattro != PrivateAttr_tp_setattro) {
                    ::AllData::all_type_setattro[type_id] = base->tp_setattro;
                }
            }
        }
    }
    {
        if (type_instance->tp_finalize != PrivateAttr_tp_finalize) {
            if (!type_instance->tp_finalize) {
                ::AllData::all_type_finalize[type_id] = get_need_tp_finalize(type_instance);
            } else {
                ::AllData::all_type_finalize[type_id] = type_instance->tp_finalize;
            }
            type_instance->tp_finalize = PrivateAttr_tp_finalize;
        } else {
            if (PyDict_ContainsString(type_instance->tp_dict, "__del__")) {
                ::AllData::all_type_finalize[type_id] = AllSlots::original_finalize;
            } else {
                PyTypeObject* base = type_instance->tp_base;
                uintptr_t base_id = (uintptr_t)(base);
                if (::AllData::all_type_finalize.find(base_id) != ::AllData::all_type_finalize.end()) {
                    ::AllData::all_type_finalize[type_id] = ::AllData::all_type_finalize[base_id];
                } else if (base && base->tp_finalize && base->tp_finalize != PrivateAttr_tp_finalize) {
                    ::AllData::all_type_finalize[type_id] = base->tp_finalize;
                }
            }
        }
    }
}

static void
ensure_subclass_tp(PyTypeObject* type_instance) noexcept
{
    // type.__subclasses__
    PyObject* type_subclasses = (PyObject*)type_instance->tp_subclasses;
    if (!type_subclasses) {
        return;
    }
    if (!PyDict_Check(type_subclasses)) {
        return;
    }
    // check all values in the dict. It should be a weakref to a type. If not, continue.
    PyObject* key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(type_subclasses, &pos, &key, &value)) {
        if (!value || !PyWeakref_CheckRef(value)) {
            continue;
        }
// python<3.13 use PyWeakref_GetObject, python>=3.13 use PyWeakref_GetRef
#if PY_VERSION_HEX < 0x030D0000
        PyObject* subclass = PyWeakref_GetObject(value);
        if (!subclass || !PyType_Check(subclass)) {
            continue;
        }
        ensure_tp((PyTypeObject*)subclass);
#else
        PyObject* subclass;
        if (PyWeakref_GetRef(value, (PyObject**)&subclass) != 1) {
            continue;
        }
        if (!subclass || !PyType_Check(subclass)) {
            continue;
        }
        ensure_tp((PyTypeObject*)subclass);
        Py_XDECREF(subclass);
#endif
    }
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
    if (PyDict_ContainsString(type_instance->tp_dict, "__getattribute__") || PyDict_ContainsString(type_instance->tp_dict, "__getattr__")) {
        ::AllData::all_type_getattro[type_id] = AllSlots::original_getattro;
    }
    if (PyDict_ContainsString(type_instance->tp_dict, "__setattr__") || PyDict_ContainsString(type_instance->tp_dict, "__delattr__")) {
        ::AllData::all_type_setattro[type_id] = AllSlots::original_setattro;
    }
    if (PyDict_ContainsString(type_instance->tp_dict, "__del__")) {
        ::AllData::all_type_finalize[type_id] = AllSlots::original_finalize;
    }

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
            try {
                final_key = custom_random_string(type_id, key, data.private_func);
            } catch (RestorePythonException& e) {
                e.restore();
                return false;
            }
        } else {
            final_key = default_random_string(type_id, key);
        }
        Py_INCREF(value);
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
                try {
                    final_key = custom_random_string(type_id, key, data.private_func);
                } catch (RestorePythonException& e) {
                    e.restore();
                    return false;
                }
            } else {
                final_key = default_random_string(type_id, key);
            }
            Py_INCREF(value);
            ::AllData::all_type_subclass_attr[i][type_id][final_key] = value;
        }
    }

    if (data.private_func) {
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
        if (!now_code || !attr_classify(typ_id, name_str, now_code)) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            Py_XDECREF(now_code);
            return NULL;
        }
        Py_XDECREF(now_code);
        return type_getattr(cls, name_str);
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

static int
init_all_slots() noexcept
{
    AllSlots::original_getattro = python_original_tp_getattro;
    AllSlots::original_setattro = python_original_tp_setattro;
    AllSlots::original_finalize = python_original_tp_finalize;
    return 0;
}

static getattrofunc
get_need_tp_getattro(PyTypeObject* cls) noexcept
{
    if (PyDict_ContainsString(cls->tp_dict, "__getattribute__") || PyDict_ContainsString(cls->tp_dict, "__getattr__")) {
        return AllSlots::original_getattro;
    }
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
    if (PyDict_ContainsString(cls->tp_dict, "__setattr__") || PyDict_ContainsString(cls->tp_dict, "__delattr__")) {
        return AllSlots::original_setattro;
    }
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

static destructor
get_need_tp_finalize(PyTypeObject* cls) noexcept
{
    if (PyDict_ContainsString(cls->tp_dict, "__del__")) {
        return AllSlots::original_finalize;
    }
    PyTypeObject* base = cls->tp_base;
    while (base) {
        if (base->tp_finalize != PrivateAttr_tp_finalize) {
            return base->tp_finalize;
        } else if (::AllData::all_type_finalize.find((uintptr_t)base) != ::AllData::all_type_finalize.end()) {
            if (::AllData::all_type_finalize[(uintptr_t)base]) {
                return ::AllData::all_type_finalize[(uintptr_t)base];
            }
            base = base->tp_base;
        } else {
            base = base->tp_base;
        }
    }
    return NULL;
}

static void
type_change_all_slots(PyTypeObject* cls, const char* name) noexcept
{
    if (strcmp(name, "__getattribute__") == 0 || strcmp(name, "__getattr__") == 0) {
        cls->tp_getattro = PrivateAttr_tp_getattro;
        ::AllData::all_type_getattro[(uintptr_t)cls] = get_need_tp_getattro(cls);
    } else if (strcmp(name, "__setattr__") == 0 || strcmp(name, "__delattr__") == 0) {
        cls->tp_setattro = PrivateAttr_tp_setattro;
        ::AllData::all_type_setattro[(uintptr_t)cls] = get_need_tp_setattro(cls);
    } else if (strcmp(name, "__del__") == 0) {
        cls->tp_finalize = PrivateAttr_tp_finalize;
        ::AllData::all_type_finalize[(uintptr_t)cls] = get_need_tp_finalize(cls);
    }
}

static void
subtype_change_all_slots(PyTypeObject* cls, const char* name) noexcept
{
    // tp_subclasses: {id(type): weakref.ref(type)}
    PyObject* tp_subclasses = (PyObject*)cls->tp_subclasses;
    if (!tp_subclasses) {
        return;
    }
    if (!PyDict_Check(tp_subclasses)) {
        return;
    }
    PyObject* key;
    PyObject* value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(tp_subclasses, &pos, &key, &value)) {
        if (!PyWeakref_Check(value)) {
            continue;
        }
#if PY_VERSION_HEX < 0x030D0000
        PyObject* obj = PyWeakref_GET_OBJECT(value);
        if (!obj) {
            continue;
        }
#else
        PyObject* obj;
        if (PyWeakref_GetRef(value, &obj) != 1) {
            continue;
        }
        if (!obj) {
            continue;
        }
#endif
        if (!PyType_Check(obj)) {
            continue;
        }
        PyTypeObject* sub_cls = (PyTypeObject*)obj;
        type_change_all_slots(sub_cls, name);
#if PY_VERSION_HEX >= 0x030D0000
        Py_XDECREF(obj);
#endif
    }
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
        if (!now_code || !attr_classify(typ_id, name_str, now_code)) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            Py_XDECREF(now_code);
            return -1;
        }
        Py_XDECREF(now_code);
        return type_setattr(cls, name_str, value);
    }
    Py_XDECREF(now_code);
    // if name in __getattribute__, __setattr__, __delattr__, __del__, just set to tp_dict
    if (strcmp(name_str.c_str(), "__getattribute__") == 0 ||
        strcmp(name_str.c_str(), "__getattr__") == 0 ||
        strcmp(name_str.c_str(), "__setattr__") == 0 ||
        strcmp(name_str.c_str(), "__delattr__") == 0 ||
        strcmp(name_str.c_str(), "__del__") == 0) {
        PyObject* tp_dict = ((PyTypeObject*)cls)->tp_dict;
        if (!tp_dict) {
            PyErr_SetString(PyExc_TypeError, "type has no tp_dict");
            return -1;
        }
        if (!value) {
            if (PyDict_DelItem(tp_dict, name) < 0) {
                PyErr_Format(PyExc_AttributeError, "type object '%.100s' has no attribute '%U'", ((PyTypeObject*)cls)->tp_name, name);
                return -1;
            }
            type_change_all_slots((PyTypeObject*)cls, name_str.c_str());
            subtype_change_all_slots((PyTypeObject*)cls, name_str.c_str());
            return 0;
        }
        if (PyDict_SetItem(tp_dict, name, value) < 0) {
            return -1;
        }
        type_change_all_slots((PyTypeObject*)cls, name_str.c_str());
        subtype_change_all_slots((PyTypeObject*)cls, name_str.c_str());
        return 0;
    }
    PyTypeObject* base = Py_TYPE(cls)->tp_base;
    while (base && base->tp_base && base->tp_setattro == PrivateAttrType_setattr) {
        base = base->tp_base;
    }
    if (!base) {
        int result = PyType_Type.tp_setattro(cls, name, value);
        ensure_tp((PyTypeObject*)cls);
        ensure_subclass_tp((PyTypeObject*)cls);
        return result;
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
        for (auto& [id, obj] : ::AllData::type_allowed_code_map[typ_id]) {
            Py_XDECREF(obj);
        }
        ::AllData::type_allowed_code_map.erase(typ_id);
    }
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        auto& need_call = ::AllData::type_need_call[typ_id];
        Py_XDECREF(need_call);
        ::AllData::type_need_call.erase(typ_id);
    }
    if (::AllData::type_attr_dict.find(typ_id) != ::AllData::type_attr_dict.end()) {
        auto& private_attrs = ::AllData::type_attr_dict[typ_id];
        for (auto& attr : private_attrs) {
            Py_XDECREF(attr.second);
        }
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
                auto& private_attrs = ::AllData::all_type_subclass_attr[parent_id][typ_id];
                for (auto& attr : private_attrs) {
                    Py_XDECREF(attr.second);
                }
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
    if (::AllData::all_type_finalize.find(typ_id) != ::AllData::all_type_finalize.end()) {
        ::AllData::all_type_finalize.erase(typ_id);
    }
    ::AllData::all_type_mutex.erase(typ_id);
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
    PyObject* name = PyUnicode_FromString("PrivateAttrBase");
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
    PyDict_SetItemString(dict, "__doc__", PyUnicode_FromString(PrivateAttrBase_doc));
    PyDict_SetItemString(dict, "__module__", PyUnicode_FromString("private_attribute"));
    PyObject *args = PyTuple_Pack(3, name, bases, dict);
    PyObject* base_type;
    if (args) {
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
    "private_temp",    // tp_name
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

static void
register_finalize(PyObject* cls) noexcept
{
    Py_ssize_t original_ref = Py_REFCNT(cls);
    PyTypeObject* base = Py_TYPE(cls)->tp_base;
    while (base && base->tp_finalize == register_finalize) {
        base = base->tp_base;
    }
    if (base && base->tp_finalize) base->tp_finalize(cls);
    if (Py_REFCNT(cls) != original_ref) {
        return;
    }
    PrivateAttrType_finalize(cls);
}


static PyObject*
metaclass_weakref_callback(PyObject* self, PyObject* /*weakref*/) noexcept
{
    void* pointer = PyCapsule_GetPointer(self, "metaclass_id");
    if (!pointer) {
        Py_RETURN_NONE;
    }
    uintptr_t id = (uintptr_t)pointer;

    std::unique_lock lock(::AllData::all_register_new_metaclass_mutex);

    auto it = ::AllData::all_register_type_weak_ref.find(id);
    if (it != ::AllData::all_register_type_weak_ref.end()) {
        Py_DECREF(it->second);
        ::AllData::all_register_type_weak_ref.erase(it);
    }

    Py_RETURN_NONE;
}

static PyMethodDef weakref_callback_def = {
    "metaclass_weakref_callback",
    (PyCFunction)metaclass_weakref_callback,
    METH_O,
    NULL
};

static PyObject*
register_metaclass(PyObject* /*self*/, PyObject* metaclass) noexcept
{
    if (!PyType_Check(metaclass)) {
        PyErr_SetString(PyExc_TypeError, "metaclass must be a type");
        return NULL;
    }
    if (!PyObject_IsSubclass(metaclass, (PyObject*)&PyType_Type)) {
        PyErr_SetString(PyExc_TypeError, "metaclass must be a metatype");
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
    ((PyTypeObject*)metaclass)->tp_getattro = PrivateAttrType_getattr;
    ((PyTypeObject*)metaclass)->tp_setattro = PrivateAttrType_setattr;
    ((PyTypeObject*)metaclass)->tp_finalize = register_finalize;
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
        ((PyTypeObject*)metaclass)->tp_getattro = PrivateAttrType_getattr;
        ((PyTypeObject*)metaclass)->tp_setattro = PrivateAttrType_setattr;
        ((PyTypeObject*)metaclass)->tp_finalize = register_finalize;
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
    PyList_Append(attr_list, PyUnicode_InternFromString("PrivateWrapProxy"));
    PyList_Append(attr_list, PyUnicode_InternFromString("PrivateAttrType"));
    PyList_Append(attr_list, PyUnicode_InternFromString("PrivateAttrBase"));
    PyList_Append(attr_list, PyUnicode_InternFromString("prepare"));
    PyList_Append(attr_list, PyUnicode_InternFromString("postprocess"));
    PyList_Append(attr_list, PyUnicode_InternFromString("register_metaclass"));
    PyList_Append(attr_list, PyUnicode_InternFromString("ensure_type"));
    PyList_Append(attr_list, PyUnicode_InternFromString("ensure_metaclass"));
    PyObject* result = PySequence_Concat(parent_dir, attr_list);
    Py_DECREF(parent_dir);
    Py_DECREF(attr_list);
    return result;
}

static int
PrivateModule_setattro(PyObject* cls, PyObject* name, PyObject* value) noexcept
{
    // if name is "__class__" it do nothing and return success
    if (PyUnicode_Check(name)) {
        const char* name_cstr = PyUnicode_AsUTF8(name);
        if (name_cstr && strcmp(name_cstr, "__class__") == 0) {
            return 0;
        }
    }
    return PyObject_GenericSetAttr(cls, name, value);
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

static PyTypeObject PrivateModuleType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute_module", //tp_name
    PyModule_Type.tp_basicsize + 8, //tp_basicsize   size of module object + 8 bytes for basicsize to avoid changing attribute '__class__'
    0, //tp_itemsize
    0, //tp_dealloc
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
    (setattrofunc)PrivateModule_setattro, //tp_setattro
    0, //tp_as_buffer
    Py_TPFLAGS_DEFAULT, //tp_flags
    0, //tp_doc
    0, //tp_traverse
    0, //tp_clear
    0, //tp_richcompare
    0, //tp_weaklistoffset
    0, //tp_iter
    0, //tp_iternext
    PrivateModule_methods_def, //tp_methods
    0, //tp_members
    PrivateModule_getsetters, //tp_getset
    &PyModule_Type, //tp_base
};

static const char* module_doc = R"(
A module that provides a metaclass for creating classes with private attributes.
Private attributes are defined in the `__private_attrs__` sequence and are only
You can use the `PrivateAttrBase` metaclass to create classes with private attributes.
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

PyMODINIT_FUNC
PyInit_private_attribute(void) noexcept
{
    if (init_all_slots() <0 ||
        PyType_Ready(&PrivateWrapType) < 0 ||
        PyType_Ready(&PrivateWrapProxyType) < 0 ||
        PyType_Ready(&PrivateAttrType) < 0 ||
        PyType_Ready(&PrivateModuleType) < 0 ||
        PyType_Ready(&PrivateTempType) < 0) {
        return NULL;
    }
    PyObject* all = PyList_New(0);
    if (!all) {
        return NULL;
    }
    PyObject* m = PyModule_Create(&def);
    if (!m) {
        Py_DECREF(all);
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
    PyList_Append(all, PyUnicode_InternFromString("PrivateWrapProxy"));
    PyList_Append(all, PyUnicode_InternFromString("PrivateAttrType"));
    PyList_Append(all, PyUnicode_InternFromString("PrivateAttrBase"));
    PyList_Append(all, PyUnicode_InternFromString("prepare"));
    PyList_Append(all, PyUnicode_InternFromString("postprocess"));
    PyList_Append(all, PyUnicode_InternFromString("register_metaclass"));
    PyList_Append(all, PyUnicode_InternFromString("ensure_type"));
    PyList_Append(all, PyUnicode_InternFromString("ensure_metaclass"));
    Py_SET_TYPE(m, &PrivateModuleType);
    return m;
}
