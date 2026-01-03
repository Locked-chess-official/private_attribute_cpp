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

static const auto module_running_time = std::chrono::system_clock::now();

static std::string
time_to_string()
{
    std::time_t original_time = std::chrono::system_clock::to_time_t(module_running_time);
    std::tm original_tm = *std::localtime(&original_time);
    std::stringstream ss;
    ss << std::put_time(&original_tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static const auto module_running_time_string = time_to_string();

class AllPyobjectAttrCacheKey
{
private:
    uintptr_t obj_id;
    std::string attr_onehash;
    std::string another_string_hash;
public:
    AllPyobjectAttrCacheKey(uintptr_t obj_id, std::string attr_name) : obj_id(obj_id) {
        std::string one_name = "_" + std::to_string(obj_id) + "_" + attr_name;
        std::string another_name = "_" + module_running_time_string + attr_name;
        picosha2::hash256_hex_string(one_name, attr_onehash);
        picosha2::hash256_hex_string(another_name, another_string_hash);
    }

    std::size_t gethash() const {
        std::size_t h1 = std::hash<uintptr_t>{}(obj_id);
        std::size_t h2 = std::hash<std::string>{}(attr_onehash);
        std::size_t h3 = std::hash<std::string>{}(another_string_hash);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }

    bool operator==(const AllPyobjectAttrCacheKey& other) const {
        return this->obj_id == other.obj_id && this->attr_onehash == other.attr_onehash && this->another_string_hash == other.another_string_hash;
    }
};

class TwoStringTuple {
private:
    std::string first;
    std::string second;
public:
    TwoStringTuple(std::string first, std::string second) : first(first), second(second) {}
    bool operator==(const TwoStringTuple& other) const {
        return this->first == other.first && this->second == other.second;
    }

    std::size_t gethash() const {
        std::size_t h1 = std::hash<std::string>{}(first);
        std::size_t h2 = std::hash<std::string>{}(second);
        return h1 ^ (h2 << 1);
    }
};

namespace std {
    template<>
    struct hash<AllPyobjectAttrCacheKey> {
        std::size_t operator()(const AllPyobjectAttrCacheKey& key) const {
            return key.gethash();
        }
    };

    template<>
    struct hash<TwoStringTuple> {
        std::size_t operator()(const TwoStringTuple& key) const {
            return key.gethash();
        }
    };
};

namespace {
    namespace AllData {
        static std::unordered_map<AllPyobjectAttrCacheKey, std::string> cache;
        static std::unordered_map<uintptr_t, std::vector<AllPyobjectAttrCacheKey>> obj_attr_keys;
        static std::shared_mutex cache_mutex;
        namespace {
            static std::unordered_map<uintptr_t, std::unordered_map<std::string, PyObject*>> type_attr_dict;
        };
        static std::unordered_map<uintptr_t, std::vector<PyCodeObject*>> type_allowed_code;
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
        static std::vector<PyTypeObject*> all_register_new_metaclass;
    };
};

struct FinalObject {
    PyObject* result = NULL;
    int status = 0;
    FinalObject(PyObject* result)
        : result(result) {
            Py_INCREF(result);
        }
    FinalObject(int status): status(status) {}
    ~FinalObject() {
        if (result) {
            Py_DECREF(result);
        }
    }
};

static TwoStringTuple get_string_hash_tuple2(std::string name);
static PyCodeObject* get_now_code();
static std::vector<PyCodeObject*>::iterator find_code(std::vector<PyCodeObject*>& code_vector, PyCodeObject* code);
static uintptr_t type_set_attr_long_long_guidance(uintptr_t type, std::string name);
static bool type_private_attr(uintptr_t type, std::string name);
static FinalObject type_get_final_attr(uintptr_t type_id, std::string name);

static bool
is_class_code(uintptr_t typ_id, PyCodeObject* code)
{
    if (::AllData::type_allowed_code.find(typ_id) == ::AllData::type_allowed_code.end()){
        return false;
    }
    auto code_list = ::AllData::type_allowed_code[typ_id];
    if (find_code(code_list, code) != code_list.end()){
        return true;
    }
    return false;
}

static bool
is_subclass_code(uintptr_t typ_id, PyCodeObject* code)
{
    std::vector<uintptr_t> parent_ids;
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()){
        parent_ids = ::AllData::all_type_parent_id[typ_id];
        for (auto& parent_id : parent_ids){
            if (is_class_code(parent_id, code)){
                return true;
            }
        }
    }
    return false;
}

static std::vector<PyCodeObject*>::iterator
find_code(std::vector<PyCodeObject*>& code_vector, PyCodeObject* code)
{
    for (auto it = code_vector.begin(); it != code_vector.end(); it++) {
        auto now_code = *it;
        uintptr_t now_code_id = (uintptr_t)now_code;
        uintptr_t code_id = (uintptr_t)code;
        if (now_code_id == code_id) {
            return it;
        }
    }
    return code_vector.end();
}

static std::string
generate_private_attr_name(uintptr_t obj_id, const std::string& attr_name)
{
    std::string combined = std::to_string(obj_id) + "_" + attr_name;
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
default_random_string(uintptr_t obj_id, std::string attr_name)
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
            int i = 1;
            std::unique_lock<std::shared_mutex> lock2(::AllData::cache_mutex);
            auto it = ::AllData::cache.find(key); // twice check
            if (it != ::AllData::cache.end()) {
                result = it->second;
                return result;
            }
            while (true) {
                bool need_break = true;
                for (auto& [k, v]: ::AllData::cache) {
                    if (v == result) {
                        result = original_result + "_" + std::to_string(i);
                        need_break = false;
                        break;
                    }
                }
                if (need_break) {
                    break;
                } else {
                    i++;
                }
            }
            if (::AllData::obj_attr_keys.find(obj_id) == ::AllData::obj_attr_keys.end()) {
                ::AllData::obj_attr_keys[obj_id] = {};
            }
            ::AllData::obj_attr_keys[obj_id].push_back(key);
            ::AllData::cache[key] = result;
        }
    }
    return result;
}

class RestorePythonException : public std::exception
{
public:
    RestorePythonException(PyObject* type, PyObject* value, PyObject* traceback)
        : type(type), value(value), traceback(traceback) {
    }

    ~RestorePythonException() {
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

    void restore() {
        PyErr_Restore(type, value, traceback);
        type = value = traceback = nullptr;
    }

private:
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
};

static std::string
custom_random_string(uintptr_t obj_id, std::string attr_name, PyObject* func)
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

            // 清理
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
                int i = 1;
                std::unique_lock<std::shared_mutex> lock2(::AllData::cache_mutex);
                auto it = ::AllData::cache.find(key); // twice check
                if (it != ::AllData::cache.end()) {
                    result = it->second;
                    return result;
                }
                while (true) {
                    bool need_break = true;
                    for (auto& [k, v]: ::AllData::cache) {
                        if (v == result) {
                            result = original_result + "_" + std::to_string(i);
                            need_break = false;
                            break;
                        }
                    }
                    if (need_break) {
                        break;
                    } else {
                        i++;
                    }
                }
                if (::AllData::obj_attr_keys.find(obj_id) == ::AllData::obj_attr_keys.end()) {
                    ::AllData::obj_attr_keys[obj_id] = {};
                }
                ::AllData::obj_attr_keys[obj_id].push_back(key);
                ::AllData::cache[key] = result;
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
clear_obj(uintptr_t obj_id)
{
    std::unique_lock<std::shared_mutex> lock(::AllData::cache_mutex);
    auto it = ::AllData::obj_attr_keys.find(obj_id);
    if (it != ::AllData::obj_attr_keys.end()) {
        for (auto& key: it->second) {
            ::AllData::cache.erase(key);
        }
        ::AllData::obj_attr_keys.erase(it);
    }
}

static PyObject*
id_getattr(std::string attr_name, PyObject* obj, PyObject* typ)
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

    if (result && PyObject_HasAttrString((PyObject*)PyObject_Type(result), "__get__") && PyObject_HasAttrString((PyObject*)PyObject_Type(result), "__set__")) {
        PyObject* python_result = PyObject_CallMethod(result, "__get__", "(OO)", obj, typ);
        return python_result;
    }
    {
        std::shared_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[final_id][obj_id]);
        if (::AllData::all_object_attr[final_id][obj_id].find(obj_private_name) != ::AllData::all_object_attr[final_id][obj_id].end()) {
            PyObject* python_obj = ::AllData::all_object_attr[final_id][obj_id][obj_private_name];
            Py_XINCREF(python_obj);
            return python_obj;
        }
    }
    if (result && PyObject_HasAttrString((PyObject*)PyObject_Type(result), "__get__")) {
        PyObject* python_result = PyObject_CallMethod(result, "__get__", "(OO)", obj, typ);
        return python_result;
    }
    if (result) {
        Py_INCREF(result);
        return result;
    }
    std::string type_name = ((PyTypeObject*)typ)->tp_name;
    std::string exception_information = "'" + type_name + "' object has no attribute '" + attr_name + "'";
    PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
    return NULL;
}

static PyObject*
type_getattr(PyObject* typ, std::string attr_name)
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
        if (PyObject_HasAttrString((PyObject*)PyObject_Type(result), "__get__")) {
            PyObject* python_result = PyObject_CallMethod(result, "__get__", "OO", Py_None, typ);
            return python_result;
        } else {
            Py_INCREF(result);
            return result;
        }
    }
    std::string type_name = ((PyTypeObject*)typ)->tp_name;
    std::string message = "type '" + type_name + "' has no attribute '" + attr_name + "'";
    PyErr_SetString(PyExc_AttributeError, message.c_str());
    return NULL;
}

static int
id_setattr(std::string attr_name, PyObject* obj, PyObject* typ, PyObject* value)
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
    if (result && PyObject_HasAttrString((PyObject*)PyObject_Type(result), "__set__")) {
        PyObject* python_result = PyObject_CallMethod(result, "__set__", "(OO)", obj, value);
        if (!python_result) {
            return -1;
        }
        Py_DECREF(python_result);
        return 0;
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

static int type_delattr(PyObject* typ, std::string attr_name);

static int
type_setattr(PyObject* typ, std::string attr_name, PyObject* value)
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
id_delattr(std::string attr_name, PyObject* obj, PyObject* typ)
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
    if (result && PyObject_HasAttrString(result, "__delete__")) {
        PyObject* delete_result = PyObject_CallMethod(result, "__delete__", "(O)", result);
        if (delete_result == NULL) {
            return -1;
        }
        Py_XDECREF(delete_result);
        return 0;
    }
    // second: delete attribute on obj
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[final_id][obj_id]);
        if (::AllData::all_object_attr[final_id][obj_id].find(obj_private_name) == ::AllData::all_object_attr[final_id][obj_id].end()) {
            lock.release();
            std::string type_name = ((PyTypeObject*)typ)->tp_name;
            std::string exception_information = "'" + type_name + "' object has no attribute '" + attr_name + "'";
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
type_delattr(PyObject* typ, std::string attr_name)
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
            std::string type_name = ((PyTypeObject*)typ)->tp_name;
            std::string exception_information = "'" + type_name + "' object has no attribute '" + attr_name + "'";
            PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
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
            std::string type_name = ((PyTypeObject*)typ)->tp_name;
            std::string exception_information = "'" + type_name + "' object has no attribute '" + attr_name + "'";
            PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
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
typedef struct PrivateWrapObject {
    PyObject_HEAD
    PyObject *result;
    PyObject *func_list;
    PyObject *decorator;
} PrivateWrapObject;

static PrivateWrapObject* PrivateWrap_New(PyObject *decorator, PyObject *func, PyObject *list);
static void PrivateWrap_dealloc(PrivateWrapObject *self);
static PyObject* PrivateWrap_call(PrivateWrapObject *self, PyObject *args, PyObject *kw);

static PyObject *
PrivateWrap_result(PyObject *obj, void *closure)
{
    if (!obj) {
        Py_RETURN_NONE;
    }

    PyObject *res = ((PrivateWrapObject*)obj)->result;
    Py_INCREF(res);
    return res;
}

static PyObject*
PrivateWrap_doc(PyObject *obj, void *closure)
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
PrivateWrap_module(PyObject *obj, void *closure)
{
    if (!obj) {
        return PyUnicode_FromString("private_attribute_cpp");
    }
    PyObject* module = PyObject_GetAttrString(((PrivateWrapObject*)obj)->result, "__module__");
    if (!module){
        PyErr_Clear();
        return PyUnicode_FromString("private_attribute_cpp");
    }
    return module;
}

static PyObject*
PrivateWarp_name(PyObject* obj, void *closure)
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
PrivateWrap_qualname(PyObject* obj, void *closure)
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
PrivateWrap_annotate(PyObject* obj, void *closure)
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
PrivateWrap_type_params(PyObject* obj, void *closure)
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
PrivateWrap_GetAttr(PyObject* obj, PyObject* args) {
    PyObject* name;
    if (!PyArg_ParseTuple(args, "O", &name)) {
        return NULL;
    }
    PyObject* res = PyObject_GetAttr(((PrivateWrapObject*)obj)->result, name);
    if (!res) {
        return NULL;
    }
    return res;
}

static PyGetSetDef PrivateWrap_getset[] = {
    {"result", (getter)PrivateWrap_result, NULL, "final result", NULL},
    {"__wrapped__", (getter)PrivateWrap_result, NULL, "final result", NULL},
    {"__doc__", (getter)PrivateWrap_doc, NULL, "doc", NULL},
    {"__module__", (getter)PrivateWrap_module, NULL, "module", NULL},
    {"__name__", (getter)PrivateWarp_name, NULL, "name", NULL},
    {"__qualname__", (getter)PrivateWrap_qualname, NULL, "qualname", NULL},
    {"__annotate__", (getter)PrivateWrap_annotate, NULL, "annotate", NULL},
    {"__type_params__", (getter)PrivateWrap_type_params, NULL, "type_params", NULL},
    {NULL}
};

static PyMethodDef PrivateWrap_methods[] = {
    {"__getattr__", (PyCFunction)PrivateWrap_GetAttr, METH_VARARGS, NULL},
    {NULL}
};

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
    0,                                 // tp_getattro
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
    PrivateWrap_methods,               // tp_methods
    0,                                 // tp_members
    PrivateWrap_getset,                // tp_getset
};

static PrivateWrapObject*
PrivateWrap_New(PyObject *decorator, PyObject *func, PyObject *list)
{
    PrivateWrapObject *self =
        PyObject_New(PrivateWrapObject, &PrivateWrapType);
    PyObject *wrapped = PyObject_CallFunctionObjArgs(decorator, func, NULL);
    if (!wrapped) {
        Py_DECREF(self);
        return NULL;
    }

    self->decorator = decorator;
    Py_INCREF(decorator);

    self->func_list = list;
    Py_INCREF(list);

    self->result = wrapped;

    return self;
}

static void
PrivateWrap_dealloc(PrivateWrapObject *self)
{
    Py_XDECREF(self->result);
    Py_XDECREF(self->func_list);
    Py_XDECREF(self->decorator);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject*
PrivateWrap_call(PrivateWrapObject *self, PyObject *args, PyObject *kw)
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

static int
PrivateWrapProxy_init(PrivateWrapProxyObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *decorator;
    PyObject *orig = NULL;

    if (!PyArg_ParseTuple(args, "O|O", &decorator, &orig))
        return -1;

    self->decorator = decorator;
    Py_INCREF(decorator);

    if (orig && PyObject_TypeCheck(orig, &PrivateWrapType)) {
        self->func_list = ((PrivateWrapObject*)orig)->func_list;
        Py_INCREF(self->func_list);
    }
    else {
        self->func_list = PyList_New(0);
    }
    return 0;
}

static PyObject*
PrivateWrapProxy_call(PrivateWrapProxyObject *self, PyObject *args, PyObject * /*kwgs */)
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

static void PrivateWrapProxy_dealloc(PrivateWrapProxyObject *self);

static PyTypeObject PrivateWrapProxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "PrivateWrapProxy",                     // tp_name
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
    "PrivateWrapProxy",                     // tp_doc
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
    (initproc)PrivateWrapProxy_init,        // tp_init
    0,                                      // tp_alloc
    PyType_GenericNew,                      // tp_new
};

static void
PrivateWrapProxy_dealloc(PrivateWrapProxyObject *self)
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

static void PrivateAttr_object_init_private_dict(uintptr_t obj_id, uintptr_t type_id);

static PyObject*
PrivateAttr_tp_getattro(PyObject* self, PyObject* name)
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t type_id = (uintptr_t)typ;
    PrivateAttr_object_init_private_dict((uintptr_t)self, type_id);
    std::string name_str = PyUnicode_AsUTF8(name);
    auto code = get_now_code();
    if (type_private_attr(type_id, name_str)) {
        if (!code || (!is_class_code(type_id, code) && !is_subclass_code(type_id, code))){
            Py_XDECREF(code);
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            return NULL;
        } else {
            Py_XDECREF(code);
            return id_getattr(name_str, self, (PyObject*)typ);
        }
    }
    Py_XDECREF(code);
    return PyObject_GenericGetAttr(self, name);
}

static int
PrivateAttr_tp_setattro(PyObject* self, PyObject* name, PyObject* value)
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
        if (!code || (!is_class_code(typ_id, code) && !is_subclass_code(typ_id, code))){
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
    return PyObject_GenericSetAttr(self, name, value);
}

static void
PrivateAttr_tp_dealloc(PyObject* self)
{
    PyTypeObject* typ = Py_TYPE(self);
    uintptr_t typ_id = (uintptr_t)typ;
    std::vector<uintptr_t> parent_ids;
    if (::AllData::all_type_parent_id.find(typ_id) != ::AllData::all_type_parent_id.end()){
        parent_ids = ::AllData::all_type_parent_id[typ_id];
    }
    uintptr_t id_self = (uintptr_t)self;
    
    {
        if (PyObject_HasAttrString((PyObject* )typ, "__del__")) {
            PyObject* del_func = PyObject_GetAttrString((PyObject* )typ, "__del__");
            PyObject* result = PyObject_CallFunctionObjArgs(del_func, self, NULL);
            Py_XDECREF(result);
            Py_XDECREF(del_func);
        }
        typ->tp_free(self);
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
PrivateAttr_object_init_private_dict(uintptr_t obj_id, uintptr_t type_id)
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

static PyObject* PrivateAttrType_new(PyTypeObject* type, PyObject* args, PyObject* kwds);
static PyObject* PrivateAttrType_getattr(PyObject* cls, PyObject* name);
static int PrivateAttrType_setattr(PyObject* cls, PyObject* name, PyObject* value);
static void PrivateAttrType_del(PyObject* cls);

static PyTypeObject PrivateAttrType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute.PrivateAttrType",    // tp_name
    sizeof(PrivateAttrTypeObject),          // tp_basicsize
    0,                                      // tp_itemsize
    (destructor)PrivateAttrType_del,        // tp_dealloc
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
    (getattrofunc)PrivateAttrType_getattr,  // tp_getattro
    (setattrofunc)PrivateAttrType_setattr,  // tp_setattro
    0,                                      // tp_as_buffer
    Py_TPFLAGS_DEFAULT,                     // tp_flags
    "metaclass for private attributes",     // tp_doc
    0,                                      // tp_traverse
    0,                                      // tp_clear
    0,                                      // tp_richcompare
    0,                                      // tp_weaklistoffset
    0,                                      // tp_iter
    0,                                      // tp_iternext
    0,                                      // tp_methods
    0,                                      // tp_members
    0,                                      // tp_getset
    &PyType_Type,                           // tp_base
    0,                                      // tp_dict
    0,                                      // tp_descr_get
    0,                                      // tp_descr_set
    0,                                      // tp_dictoffset
    0,                                      // tp_init
    0,                                      // tp_alloc
    (newfunc)PrivateAttrType_new,           // tp_new
};

static PyObject*
get_string_hash_tuple(std::string name)
{
    std::string name1;
    std::string name2;
    name1 = module_running_time_string + "_" + name;
    uintptr_t type_id = reinterpret_cast<uintptr_t>(&PrivateAttrType);
    name2 = std::to_string(type_id) + "_" + name1;
    std::string name1hash, name2hash;
    picosha2::hash256_hex_string(name1, name1hash);
    picosha2::hash256_hex_string(name2, name2hash);
    return PyTuple_Pack(2, PyUnicode_FromString(name1hash.c_str()), PyUnicode_FromString(name2hash.c_str()));
}

static TwoStringTuple
get_string_hash_tuple2(std::string name)
{
    std::string name1;
    std::string name2;
    name1 = module_running_time_string + "_" + name;
    uintptr_t type_id = reinterpret_cast<uintptr_t>(&PrivateAttrType);
    name2 = std::to_string(type_id) + "_" + name1;
    std::string name1hash, name2hash;
    picosha2::hash256_hex_string(name1, name1hash);
    picosha2::hash256_hex_string(name2, name2hash);
    return TwoStringTuple(name1hash, name2hash);
}

static FinalObject
type_get_final_attr(uintptr_t type_id, std::string name)
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
type_set_attr_long_long_guidance(uintptr_t type_id, std::string name)
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
type_private_attr(uintptr_t type_id, std::string name)
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
get_now_code()
{
    PyFrameObject* f = PyEval_GetFrame();
    if (!f) {
        return NULL;
    }
    PyCodeObject* code = PyFrame_GetCode(f);
    return code;
}

static void
analyse_all_code(PyObject* obj, std::vector<PyCodeObject*>& list, std::unordered_set<uintptr_t>& _seen)
{
    uintptr_t obj_id = (uintptr_t)obj;
    if (_seen.find(obj_id) != _seen.end()) {
        return;
    }
    _seen.insert(obj_id);
    if (PyObject_TypeCheck(obj, &PyCode_Type)) {
        Py_INCREF(obj);
        list.push_back((PyCodeObject*)obj);
        PyObject* co_contain = PyObject_GetAttrString(obj, "co_consts");
        if (co_contain && PySequence_Check(co_contain)) {
            Py_ssize_t len = PySequence_Length(co_contain);
            for (Py_ssize_t i = 0; i < len; i++) {
                PyObject* item = PySequence_GetItem(co_contain, i);
                if (item) {
                    analyse_all_code(item, list, _seen);
                } else {
                    PyErr_Clear();
                }
            }
        } else {
            PyErr_Clear();
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
                    analyse_all_code(func, list, _seen);
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
            analyse_all_code(fget, list, _seen);
        } else {
            PyErr_Clear();
        }
        PyObject* fset = PyObject_GetAttrString(obj, "fset");
        if (fset) {
            analyse_all_code(fset, list, _seen);
        } else {
            PyErr_Clear();
        }
        PyObject* fdel = PyObject_GetAttrString(obj, "fdel");
        if (fdel) {
            analyse_all_code(fdel, list, _seen);
        } else {
            PyErr_Clear();
        }
        return;
    }
    if (PyObject_TypeCheck(obj, &PyClassMethod_Type) || PyObject_TypeCheck(obj, &PyStaticMethod_Type)) {
        PyObject* func = PyObject_GetAttrString(obj, "__func__");
        if (func) {
            analyse_all_code(func, list, _seen);
        } else {
            PyErr_Clear();
        }
        return;
    }
    PyObject* wrap = PyObject_GetAttrString(obj, "__wrapped__");
    if (wrap) {
        analyse_all_code(wrap, list, _seen);
        return;
    } else {
        PyErr_Clear();
    }
    PyObject* code = PyObject_GetAttrString(obj, "__code__");
    if (code) {
        analyse_all_code(code, list, _seen);
    } else {
        PyErr_Clear();
    }
}


static std::string
real_class_name(std::string name, std::string class_name)
{
    // if the name starts with "__" but does not end with "__", change to _ClassName__name
    if (name.length() >= 2 && name.substr(0, 2) == "__" && name.substr(name.length() - 2) != "__") {
        return "_" + class_name + name;
    }
    return name;
}


static PyObject*
PrivateAttrType_new(PyTypeObject* type, PyObject* args, PyObject* kwds) 
{
    static const char* invalid_name[] = {"__private_attrs__", "__slots__", "__getattribute__", "__getattr__", "__init__",
        "__setattr__", "__delattr__", "__name__", "__module__", "__doc__", "__getstate__", "__setstate__",
        "__get__", "__set__", "__delete__", "__new__", "__set_name__", "__class__", NULL};

    PyObject* name;
    PyObject* bases;
    PyObject* attrs;
    PyObject* private_func = NULL;
    PyObject* base_kwds = NULL;

    // only parse name, bases, attrs
    if (!PyArg_ParseTuple(args, "OOO", &name, &bases, &attrs)) {
        return NULL;
    }

    // get "private_func" from kwds
    if (kwds && PyDict_Check(kwds)) {
        private_func = PyDict_GetItemString(kwds, "private_func");
        Py_INCREF(private_func);
        base_kwds = PyDict_Copy(kwds);
        if (!base_kwds) {
            return NULL;
        }
        PyDict_DelItemString(base_kwds, "private_func");
        PyErr_Clear();
    }

    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "name must be a string");
        return NULL;
    }
    std::string class_name = PyUnicode_AsUTF8(name);

    if (!PyTuple_Check(bases)) {
        PyErr_SetString(PyExc_TypeError, "bases must be a tuple");
        return NULL;
    }

    if (!PyDict_Check(attrs)) {
        PyErr_SetString(PyExc_TypeError, "attrs must be a dict");
        return NULL;
    }

    PyObject* __private_attrs__ = PyDict_GetItemString(attrs, "__private_attrs__");
    if (!__private_attrs__) {
        PyErr_SetString(PyExc_TypeError, "'__private_attrs__' is needed for type 'PrivateAttrType'");
        return NULL;
    }

    if (!PySequence_Check(__private_attrs__)) {
        PyErr_SetString(PyExc_TypeError, "'__private_attrs__' must be a sequence");
        return NULL;
    }

    PyObject* attrs_copy = PyDict_Copy(attrs);
    if (!attrs_copy) {
        return NULL;
    }

    Py_ssize_t private_attr_len = PySequence_Length(__private_attrs__);
    if (private_attr_len < 0) {
        Py_DECREF(attrs_copy);
        return NULL;
    }

    PyObject* new_hash_private_attrs = PyTuple_New(private_attr_len);
    std::unordered_set<TwoStringTuple> private_attrs_set;
    if (!new_hash_private_attrs) {
        Py_DECREF(attrs_copy);
        return NULL;
    }

    std::unordered_set<std::string> private_attrs_vector_string;

    for (Py_ssize_t i = 0; i < private_attr_len; i++) {
        PyObject* attr = PySequence_GetItem(__private_attrs__, i);
        if (!attr) {
            Py_DECREF(attrs_copy);
            Py_DECREF(new_hash_private_attrs);
            return NULL;
        }

        if (!PyUnicode_Check(attr)) {
            PyErr_SetString(PyExc_TypeError, "all items in '__private_attrs__' must be strings");
            Py_DECREF(attr);
            Py_DECREF(attrs_copy);
            Py_DECREF(new_hash_private_attrs);
            return NULL;
        }

        const char* attr_cstr = PyUnicode_AsUTF8(attr);
        if (!attr_cstr) {
            Py_DECREF(attr);
            Py_DECREF(attrs_copy);
            Py_DECREF(new_hash_private_attrs);
            return NULL;
        }

        std::string attr_str = real_class_name(attr_cstr, class_name);

        for (const char** p = invalid_name; *p != NULL; p++) {
            if (attr_str == *p) {
                std::string error_msg = "invalid attribute name: '" + std::string(*p) + "'";
                PyErr_SetString(PyExc_TypeError, error_msg.c_str());
                Py_DECREF(attr);
                Py_DECREF(attrs_copy);
                Py_DECREF(new_hash_private_attrs);
                return NULL;
            }
        }

        PyObject* hash_tuple = get_string_hash_tuple(attr_str);
        TwoStringTuple hash_tuple_key = get_string_hash_tuple2(attr_str);
        if (!hash_tuple) {
            Py_DECREF(attr);
            Py_DECREF(attrs_copy);
            Py_DECREF(new_hash_private_attrs);
            return NULL;
        }
        PyTuple_SET_ITEM(new_hash_private_attrs, i, hash_tuple);
        private_attrs_set.insert(hash_tuple_key);
        private_attrs_vector_string.insert(attr_str);
        Py_DECREF(attr);
    }

    if (PyDict_SetItemString(attrs_copy, "__private_attrs__", new_hash_private_attrs) < 0) {
        Py_DECREF(attrs_copy);
        Py_DECREF(new_hash_private_attrs);
        return NULL;
    }

    PyObject* all_slots = PyDict_GetItemString(attrs_copy, "__slots__");
    bool has_slots = (all_slots != NULL);
    
    if (has_slots) {
        PyObject* slot_seq = PySequence_Fast(all_slots, "__slots__ must be a sequence");
        if (!slot_seq) {
            Py_DECREF(attrs_copy);
            Py_DECREF(new_hash_private_attrs);
            return NULL;
        }

        Py_ssize_t slot_len = PySequence_Fast_GET_SIZE(slot_seq);

        for (Py_ssize_t j = 0; j < slot_len; j++) {
            PyObject* slot = PySequence_Fast_GET_ITEM(slot_seq, j);
            if (PyUnicode_Check(slot)) {
                const char* slot_cstr = PyUnicode_AsUTF8(slot);
                if (private_attrs_vector_string.find((std::string)slot_cstr) != private_attrs_vector_string.end()){
                    std::string error_msg = "'__slots__' and '__private_attrs__' cannot have the same attribute name: '" + std::string(slot_cstr) + "'";
                    PyErr_SetString(PyExc_TypeError, error_msg.c_str());
                    Py_DECREF(slot_seq);
                    Py_DECREF(attrs_copy);
                    Py_DECREF(new_hash_private_attrs);
                    return NULL;
                }
            }
        }
        Py_DECREF(slot_seq);
    }

    std::vector<uintptr_t> all_need_analyse_base;
    Py_ssize_t bases_len = PyTuple_GET_SIZE(bases);
    for (Py_ssize_t i = 0; i < bases_len; i++) {
        PyObject* base = PyTuple_GET_ITEM(bases, i);
        if (!base || !PyType_Check(base) || !PyObject_IsInstance(base, (PyObject*)&PrivateAttrType)) {
            continue;
        }
        all_need_analyse_base.push_back((uintptr_t)base);
    }
    std::unordered_map<std::string, PyObject*> need_remove_itself; // key: attr name, value: attr value
    std::unordered_map<uintptr_t, std::unordered_map<std::string, PyObject*>> need_remove_subclass; // key: type id, value: key: attr name, value: attr value
    std::function<uintptr_t(std::string)> need_remove_to_subclass = [&all_need_analyse_base](std::string attr_name){
        for (auto& base: all_need_analyse_base) {
            if (type_private_attr(base, attr_name)) {
                return type_set_attr_long_long_guidance(base, attr_name);
            }
        }
        return (uintptr_t)0;
    };
    {
        Py_ssize_t pos = 0;
        PyObject* key, *value;
        PyObject* forward_analyse = PyDict_Copy(attrs_copy);
        while (PyDict_Next(forward_analyse, &pos, &key, &value)) {
            std::string attr_name = real_class_name(PyUnicode_AsUTF8(key), class_name);
            PyObject* need_value;
            if (PyObject_IsInstance(value, (PyObject*)&PrivateWrapType)) {
                need_value = ((PrivateWrapObject*)value)->result;
            } else {
                need_value = value;
            }
            if (private_attrs_vector_string.find(attr_name) != private_attrs_vector_string.end()) {
                Py_INCREF(need_value);
                std::string final_key;
                need_remove_itself[attr_name] = need_value;
                PyDict_DelItem(attrs_copy, key);
                continue;
            }
            uintptr_t need_remove_subclass_id = need_remove_to_subclass(attr_name);
            if (need_remove_subclass_id) {
                if (need_remove_subclass.find(need_remove_subclass_id) == need_remove_subclass.end()) {
                    need_remove_subclass[need_remove_subclass_id] = {};
                }
                Py_INCREF(need_value);
                need_remove_subclass[need_remove_subclass_id][attr_name] = need_value;
                PyDict_DelItem(attrs_copy, key);
                continue;
            }
            if (value != need_value) PyDict_SetItem(attrs_copy, key, need_value);
        }
        Py_DECREF(forward_analyse);
    }

    PyObject* type_args = PyTuple_Pack(3, name, bases, attrs_copy);
    if (!type_args) {
        Py_DECREF(attrs_copy);
        Py_DECREF(new_hash_private_attrs);
        return NULL;
    }
    PyObject* new_type = type->tp_base->tp_new(type, type_args, base_kwds);
    Py_DECREF(type_args);

    if (!new_type) {
        Py_DECREF(attrs_copy);
        Py_DECREF(new_hash_private_attrs);
        return NULL;
    }
    if (!PyObject_IsInstance(new_type, (PyObject*)type)) {
        Py_DECREF(attrs_copy);
        Py_DECREF(new_hash_private_attrs);
        Py_DECREF(new_type);
        PyErr_SetString(PyExc_TypeError, ("base type creation did not return an instance of '" + std::string(type->tp_name) + "'").c_str());
        return NULL;
    }

    PyTypeObject* type_instance = (PyTypeObject*)new_type;
    uintptr_t type_id = (uintptr_t)(type_instance);
    type_instance->tp_getattro = PrivateAttr_tp_getattro;
    type_instance->tp_setattro = PrivateAttr_tp_setattro;
    type_instance->tp_dealloc = PrivateAttr_tp_dealloc;

    ::AllData::type_attr_dict[type_id] = {};
    ::AllData::all_type_attr_set[type_id] = private_attrs_set;

    // iter mro and put in all_type_parent_id
    PyObject* mro = type_instance->tp_mro;
    Py_ssize_t mro_size = PyTuple_GET_SIZE(mro);
    std::vector<uintptr_t> mro_vector;
    for (Py_ssize_t i = 1; i < mro_size; i++) {
        PyObject* item = PyTuple_GET_ITEM(mro, i);
        if (!item || !PyType_Check(item) || !PyObject_IsInstance(item, (PyObject*)&PrivateAttrType)) {
            continue;
        }
        mro_vector.push_back((uintptr_t)item);
    }
    ::AllData::all_type_parent_id[type_id] = mro_vector;

    ::AllData::type_allowed_code[type_id] = {};
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
    for (auto& [key, value]: need_remove_itself) {
        std::string final_key;
        if (private_func) {
            try {
                final_key = custom_random_string(type_id, key, private_func);
            } catch (RestorePythonException& e) {
                Py_DECREF(attrs_copy);
                Py_DECREF(new_hash_private_attrs);
                Py_DECREF(new_type);
                e.restore();
                return NULL;
            }
        } else {
            final_key = default_random_string(type_id, key);
        }
        ::AllData::type_attr_dict[type_id][final_key] = value;
    }
    for (auto& [i, map]: need_remove_subclass) {
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
            if (private_func) {
                try {
                    final_key = custom_random_string(type_id, key, private_func);
                } catch (RestorePythonException& e) {
                    Py_DECREF(attrs_copy);
                    Py_DECREF(new_hash_private_attrs);
                    Py_DECREF(new_type);
                    e.restore();
                    return NULL;
                }
            } else {
                final_key = default_random_string(type_id, key);
            }
            ::AllData::all_type_subclass_attr[i][type_id][final_key] = value;
        }
    }
    if (private_func) {
        ::AllData::type_need_call[type_id] = private_func;
        Py_INCREF(private_func);
    }

    {
        PyObject* original_key;
        Py_ssize_t original_pos = 0;
        PyObject* original_value;
        while (PyDict_Next(attrs, &original_pos, &original_key, &original_value)) {
            std::unordered_set<uintptr_t> set;
            analyse_all_code(original_value, ::AllData::type_allowed_code[type_id], set);
        }
    }

    Py_DECREF(attrs_copy);

    return new_type;
}

static PyObject*
PrivateAttrType_getattr(PyObject* cls, PyObject* name)
{
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "cls must be a type");
        return NULL;
    }
    uintptr_t typ_id = (uintptr_t)(cls);
    std::string name_str = PyUnicode_AsUTF8(name);
    PyCodeObject* now_code = get_now_code();
    if (type_private_attr(typ_id, name_str)) {
        if (!now_code || (!is_class_code(typ_id, now_code) && !is_subclass_code(typ_id, now_code))) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            Py_XDECREF(now_code);
            return NULL;
        }
        Py_XDECREF(now_code);
        return type_getattr(cls, name_str);
    }
    Py_XDECREF(now_code);
    return Py_TYPE(cls)->tp_base->tp_getattro(cls, name);
}

static int
PrivateAttrType_setattr(PyObject* cls, PyObject* name, PyObject* value)
{
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "cls must be a type");
        return -1;
    }
    uintptr_t typ_id = (uintptr_t)(cls);
    std::string name_str = PyUnicode_AsUTF8(name);
    PyCodeObject* now_code = get_now_code();
    if (type_private_attr(typ_id, name_str)) {
        if (!now_code || (!is_class_code(typ_id, now_code) && !is_subclass_code(typ_id, now_code))) {
            PyErr_SetString(PyExc_AttributeError, "private attribute");
            Py_XDECREF(now_code);
            return -1;
        }
        Py_XDECREF(now_code);
        return type_setattr(cls, name_str, value);
    }
    Py_XDECREF(now_code);
    return Py_TYPE(cls)->tp_base->tp_setattro(cls, name, value);
}

static void
PrivateAttrType_del(PyObject* cls)
{
    uintptr_t typ_id = (uintptr_t) cls;
    if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()) {
        ::AllData::all_type_attr_set.erase(typ_id);
    }
    if (::AllData::type_allowed_code.find(typ_id) != ::AllData::type_allowed_code.end()) {
        auto& allowed_code = ::AllData::type_allowed_code[typ_id];
        for (auto& code : allowed_code) {
            Py_XDECREF(code);
        }
        ::AllData::type_allowed_code.erase(typ_id);
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
    ::AllData::all_type_mutex.erase(typ_id);
    clear_obj(typ_id);
    (Py_TYPE(cls))->tp_free(cls);
}

// PrivateAttrBase
static PyObject*
create_private_attr_base_simple(void)
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
    return base_type;
}

typedef struct PrivateModule{
    PyObject_HEAD
}PrivateModule;

static PyObject*
PrivateModule_get_PrivateWrapProxy(PyObject* /*self*/, void* /*closure*/)
{
    PyObject* PythonPrivateWrapProxy = (PyObject*)&PrivateWrapProxyType;
    Py_INCREF(PythonPrivateWrapProxy);
    return PythonPrivateWrapProxy;
}

// type PrivateAttrType
static PyObject*
PrivateModule_get_PrivateAttrType(PyObject* /*self*/, void* /*closure*/)
{
    PyObject* PythonPrivateAttrType = (PyObject*)&PrivateAttrType;
    Py_INCREF(PythonPrivateAttrType);
    return PythonPrivateAttrType;
}

static PyObject*
PrivateModule_get_PrivateAttrBase(PyObject* /*self*/, void* /*closure*/)
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
PrivateModule_dir(PyObject* self)
{
    PyObject* parent_dir = PyObject_CallMethod((PyObject*)&PyModule_Type, "__dir__", "O", self);
    if (!parent_dir) return NULL;
    PyObject* attr_list = PyList_New(0);
    if (!attr_list) {
        Py_DECREF(parent_dir);
        return NULL;
    }
    PyList_Append(attr_list, PyUnicode_FromString("PrivateWrapProxy"));
    PyList_Append(attr_list, PyUnicode_FromString("PrivateAttrType"));
    PyList_Append(attr_list, PyUnicode_FromString("PrivateAttrBase"));
    PyObject* result = PySequence_Concat(parent_dir, attr_list);
    Py_DECREF(parent_dir);
    Py_DECREF(attr_list);
    return result;
}

static int
PrivateModule_setattro(PyObject* cls, PyObject* name, PyObject* value)
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

static PyGetSetDef PrivateModule_getsetters[] = {
    {"PrivateWrapProxy", (getter)PrivateModule_get_PrivateWrapProxy, NULL, NULL, NULL},
    {"PrivateAttrType", (getter)PrivateModule_get_PrivateAttrType, NULL, NULL, NULL},
    {"PrivateAttrBase", (getter)PrivateModule_get_PrivateAttrBase, NULL, NULL, NULL},
    {NULL}
};

static PyMethodDef PrivateModule_methods[] = {
    {"__dir__", (PyCFunction)PrivateModule_dir, METH_NOARGS, NULL},
    {NULL}  // Sentinel
};

static PyTypeObject PrivateModuleType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "private_attribute_module", //tp_name
    sizeof(PrivateModule), //tp_basicsize
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
    PrivateModule_methods, //tp_methods
    0, //tp_members
    PrivateModule_getsetters, //tp_getset
    &PyModule_Type, //tp_base
};

static PyModuleDef def = {
    PyModuleDef_HEAD_INIT,
    "private_attribute_cpp",
    NULL,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit_private_attribute(void)
{
    if (PyType_Ready(&PrivateWrapType) < 0 ||
        PyType_Ready(&PrivateWrapProxyType) < 0 ||
        PyType_Ready(&PrivateAttrType) < 0 ||
        PyType_Ready(&PrivateModuleType) < 0) {
        return NULL;
    }

    PyObject* m = PyModule_Create(&def);
    Py_SET_TYPE(m, &PrivateModuleType);
    return m;
}
