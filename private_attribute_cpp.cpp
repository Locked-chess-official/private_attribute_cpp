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
    long long obj_id;
    std::string attr_onehash;
    std::string another_string_hash;
public:
    AllPyobjectAttrCacheKey(long long obj_id, std::string attr_name) : obj_id(obj_id) {
        std::string one_name = "_" + std::to_string(obj_id) + "_" + attr_name;
        std::string another_name = "_" + module_running_time_string + attr_name;
        picosha2::hash256_hex_string(one_name, attr_onehash);
        picosha2::hash256_hex_string(another_name, another_string_hash);
    }

    std::size_t gethash() const {
        std::size_t h1 = std::hash<long long>{}(obj_id);
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
        static std::unordered_map<long long, std::vector<AllPyobjectAttrCacheKey>> obj_attr_keys;
        static std::shared_mutex cache_mutex;
        namespace {
            static std::unordered_map<long long, std::unordered_map<std::string, PyObject*>> type_attr_dict;
        };
        static std::unordered_map<long long, std::vector<PyCodeObject*>> type_allowed_code;
        static std::unordered_map<long long, std::shared_ptr<std::shared_mutex>> all_type_mutex;
        static std::unordered_map<long long, PyObject*> type_need_call;
        static std::unordered_map<long long, std::unordered_set<TwoStringTuple>> all_type_attr_set;
        namespace {
            static std::unordered_map<long long, std::unordered_map<long long,
            std::unordered_map<std::string, PyObject*>>> all_object_attr;
        };
        static std::unordered_map<long long, std::unordered_map<long long, std::shared_ptr<std::shared_mutex>>> all_object_mutex;
    };
};

static PyObject* id_getattr(std::string attr_name, PyObject* obj, PyObject* typ);
static int id_setattr(std::string attr_name, PyObject* obj, PyObject* typ, PyObject* value);
static int id_delattr(std::string attr_name, PyObject* obj, PyObject* typ);
static TwoStringTuple get_string_hash_tuple2(std::string name);
static PyCodeObject* get_now_code();
static std::vector<PyCodeObject*>::iterator find_code(std::vector<PyCodeObject*>& code_vector, PyCodeObject* code);
static void clear_obj(long long obj_id);

static bool
is_class_code(long long typ_id, PyCodeObject* code)
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
is_subclass_code(PyTypeObject* typ, PyCodeObject* code)
{
    PyObject* mro = typ->tp_mro;
    for (int i = 1; i < PyTuple_GET_SIZE(mro); i++) {
        PyObject* parent = PyTuple_GET_ITEM(mro, i);
        if (is_class_code((long long)(uintptr_t)parent, code)){
            return true;
        }
    }
    return false;
}

class FunctionCreator
{
private:
    PyTypeObject* typ;
public:
    FunctionCreator(PyTypeObject* typ)
        :typ(typ) {}

    PyObject* getattro(PyObject* self, PyObject* name) {
        if (!typ) {
            PyErr_SetString(PyExc_SystemError, "type is NULL");
            return NULL;
        }
        long long typ_id = (long long)(uintptr_t)typ;
        std::string name_str = PyUnicode_AsUTF8(name);
        TwoStringTuple name_hash_set = get_string_hash_tuple2(name_str);
        if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()){
            auto attr_set = ::AllData::all_type_attr_set[typ_id];
            if (attr_set.find(name_hash_set) != attr_set.end()){
                PyErr_SetObject(PyExc_AttributeError, name);
                return NULL;
            }
        }
        PyObject* mro = typ->tp_mro;
        for (int i = 1; i < PyTuple_GET_SIZE(mro); i++) {
            PyObject* parent = PyTuple_GET_ITEM(mro, i);
            long long parent_id = (long long)(uintptr_t)parent;
            if (::AllData::all_type_attr_set.find(parent_id) != ::AllData::all_type_attr_set.end()){
                auto attr_set = ::AllData::all_type_attr_set[parent_id];
                if (attr_set.find(name_hash_set) != attr_set.end()){
                    PyErr_SetObject(PyExc_AttributeError, name);
                    return NULL;
                }
            }
        }
        auto getattribute = PyObject_GetAttrString((PyObject*)typ, "__getattribute__");
        if (!getattribute) {
            PyErr_SetString(PyExc_AttributeError, "__getattribute__");
            return NULL;
        }
        return PyObject_CallFunctionObjArgs(getattribute, self, name, NULL);
    }

    PyObject* getattr(PyObject* self, PyObject* name) {
        if (!typ) {
            PyErr_SetString(PyExc_SystemError, "type is NULL");
            return NULL;
        }
        long long typ_id = (long long)(uintptr_t)typ;
        std::string name_str = PyUnicode_AsUTF8(name);
        auto code = get_now_code();
        TwoStringTuple name_hash_set = get_string_hash_tuple2(name_str);
        if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()){
            auto attr_set = ::AllData::all_type_attr_set[typ_id];
            if (attr_set.find(name_hash_set) != attr_set.end()){
                if (!code || (!is_class_code(typ_id, code) && !is_subclass_code(typ, code))){
                    PyErr_SetString(PyExc_AttributeError, "private attribute");
                    return NULL;
                } else {
                    return id_getattr(name_str, self, (PyObject*)typ);
                }
            }
        }
        PyObject* typ_mro = PyObject_GetAttrString((PyObject*)typ, "__mro__");
        // check if the attribute in parents' private attrs
        for (int i = 1; i < PyTuple_GET_SIZE(typ_mro); i++) {
            PyObject* parent = PyTuple_GET_ITEM(typ_mro, i);
            long long parent_id = (long long)(uintptr_t)parent;
            if (::AllData::all_type_attr_set.find(parent_id) != ::AllData::all_type_attr_set.end()){
                auto attr_set = ::AllData::all_type_attr_set[parent_id];
                if (attr_set.find(name_hash_set) != attr_set.end()){
                    if (!code || (!is_class_code(typ_id, code) && !is_subclass_code(typ, code))){
                        PyErr_SetString(PyExc_AttributeError, "private attribute");
                        return NULL;
                    } else {
                        return id_getattr(name_str, self, (PyObject*)parent);
                    }
                }
            }
        }
        auto getattr = PyObject_GetAttrString((PyObject*)typ, "__getattr__");
        if (getattr) {
            return PyObject_CallFunctionObjArgs(getattr, self, name, NULL);
        }
        std::string final_exc_msg = "'" + std::string(typ->tp_name) + "' object has no attribute '" + name_str + "'";
        PyErr_SetString(PyExc_AttributeError, final_exc_msg.c_str());
        PyObject *exc_typ, *exc_value, *traceback;
        PyErr_Fetch(&exc_typ, &exc_value, &traceback);
        PyObject_SetAttrString(exc_value, "obj", self);
        PyObject_SetAttrString(exc_value, "name", PyUnicode_FromString(name_str.c_str()));
        PyErr_Restore(exc_typ, exc_value, traceback);
        return NULL;
    }

    int setattro(PyObject* self, PyObject* name, PyObject* value) {
        if (!typ) {
            PyErr_SetString(PyExc_SystemError, "type is NULL");
            return -1;
        }
        long long typ_id = (long long)(uintptr_t)typ;
        const char* c_name = PyUnicode_AsUTF8(name);
            if (!c_name) {
                return -1;
            }
        std::string name_str(c_name);
        TwoStringTuple name_hash_set = get_string_hash_tuple2(name_str);
        auto code = get_now_code();
        if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()){
            auto attr_set = ::AllData::all_type_attr_set[typ_id];
            if (attr_set.find(name_hash_set) != attr_set.end()){
                if (!code || (!is_class_code(typ_id, code) && !is_subclass_code(typ, code))){
                    PyErr_SetString(PyExc_AttributeError, "private attribute");
                    return -1;
                } else {
                    return id_setattr(name_str, self, (PyObject*)typ, value);
                }
            }
        }
        PyObject* typ_mro = typ->tp_mro;
        // check if the attribute in parents' private attrs
        for (int i = 1; i < PyTuple_GET_SIZE(typ_mro); i++) {
            PyObject* parent = PyTuple_GET_ITEM(typ_mro, i);
            long long parent_id = (long long)(uintptr_t)parent;
            if (::AllData::all_type_attr_set.find(parent_id) != ::AllData::all_type_attr_set.end()){
                auto attr_set = ::AllData::all_type_attr_set[parent_id];
                if (attr_set.find(name_hash_set) != attr_set.end()){
                    if (!code || (!is_class_code(typ_id, code) && !is_subclass_code(typ, code))){
                        PyErr_SetString(PyExc_AttributeError, "private attribute");
                        return -1;
                    } else {
                        return id_setattr(name_str, self, (PyObject*)parent, value);
                    }
                }
            }
        }
        return PyObject_GenericSetAttr(self, name, value);
    }

    int delattr(PyObject* self, PyObject* name) {
        if (!typ) {
            PyErr_SetString(PyExc_SystemError, "type is NULL");
            return -1;
        }
        long long typ_id = (long long)(uintptr_t)typ;
        std::string name_str = PyUnicode_AsUTF8(name);
        TwoStringTuple name_hash_set = get_string_hash_tuple2(name_str);
        auto code = get_now_code();
        if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()){
            auto attr_set = ::AllData::all_type_attr_set[typ_id];
            if (attr_set.find(name_hash_set) != attr_set.end()){
                if (!code || (!is_class_code(typ_id, code) && !is_subclass_code(typ, code))){
                    PyErr_SetString(PyExc_AttributeError, "private attribute");
                    return -1;
                } else {
                    return id_delattr(name_str, self, (PyObject*)typ);
                }
            }
        }
        PyObject* typ_mro = typ->tp_mro;
        // check if the attribute in parents' private attrs
        for (int i = 1; i < PyTuple_GET_SIZE(typ_mro); i++) {
            PyObject* parent = PyTuple_GET_ITEM(typ_mro, i);
            long long parent_id = (long long)(uintptr_t)parent;
            if (::AllData::all_type_attr_set.find(parent_id) != ::AllData::all_type_attr_set.end()){
                auto attr_set = ::AllData::all_type_attr_set[parent_id];
                if (attr_set.find(name_hash_set) != attr_set.end()){
                    if (!code || (!is_class_code(typ_id, code) && !is_subclass_code(typ, code))){
                        PyErr_SetString(PyExc_AttributeError, "private attribute");
                        return -1;
                    } else {
                        return id_delattr(name_str, self, (PyObject*)parent);
                    }
                }
            }
        }
        return PyObject_GenericSetAttr(self, name, NULL);
    }

    void del(PyObject* self) {
        if (!typ) {
            PyErr_SetString(PyExc_SystemError, "type is NULL");
            return;
        }
        PyObject* typ_mro = typ->tp_mro;
        long long id_self = (long long)(uintptr_t)self;
        long long typ_id = (long long)(uintptr_t)typ;
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
            for (int i = 1; i < PyTuple_GET_SIZE(typ_mro); i++) {
                PyObject* parent = PyTuple_GET_ITEM(typ_mro, i);
                long long parent_id = (long long)(uintptr_t)parent;
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
};

namespace {
    namespace AllData {
        static std::unordered_map<long long, std::shared_ptr<FunctionCreator>> all_function_creator;
    };
};

static std::vector<PyCodeObject*>::iterator
find_code(std::vector<PyCodeObject*>& code_vector, PyCodeObject* code)
{
    for (auto it = code_vector.begin(); it != code_vector.end(); it++) {
        auto now_code = *it;
        long long now_code_id = (long long)(uintptr_t)now_code;
        long long code_id = (long long)(uintptr_t)code;
        if (now_code_id == code_id) {
            return it;
        }
    }
    return code_vector.end();
}

static std::string
generate_private_attr_name(long long obj_id, const std::string& attr_name)
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
default_random_string(long long obj_id, std::string attr_name)
{
    AllPyobjectAttrCacheKey key(obj_id, attr_name);
    std::string result;
    {
        std::shared_lock<std::shared_mutex> lock(::AllData::cache_mutex);
        auto it = ::AllData::cache.find(key);
        if (it != ::AllData::cache.end()) {
            result = it->second;
        } else {
            result = generate_private_attr_name(obj_id, attr_name);
            std::string original_result = result;
            int i = 1;
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
custom_random_string(long long obj_id, std::string attr_name, PyObject* func)
{
    PyObject* args;
    PyObject* python_obj_id = PyLong_FromLong(obj_id);
    PyObject* python_attr_name = PyUnicode_FromString(attr_name.c_str());
    AllPyobjectAttrCacheKey key(obj_id, attr_name);
    std::string result;
    {
        std::shared_lock<std::shared_mutex> lock(::AllData::cache_mutex);
        auto it = ::AllData::cache.find(key);
        if (it != ::AllData::cache.end()) {
            result = it->second;
        } else {
            args = PyTuple_New(2);
            PyTuple_SetItem(args, 0, python_obj_id);
            PyTuple_SetItem(args, 1, python_attr_name);
            PyObject* python_result = PyObject_CallObject((PyObject*)func, args);
            if (python_result) {
                if (!PyUnicode_Check(python_result)) {
                    Py_DECREF(python_result);
                    PyErr_SetString(PyExc_TypeError, "Function must return a string");
                    PyObject *type, *value, *traceback;
                    PyErr_Fetch(&type, &value, &traceback);
                    throw RestorePythonException(type, value, traceback);
                }
                result = PyUnicode_AsUTF8(python_result);
                Py_DECREF(python_result);
                std::string original_result = result;
                int i = 1;
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
clear_obj(long long obj_id)
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
    long long obj_id, typ_id;
    obj_id = (long long) (uintptr_t) obj;
    typ_id = (long long) (uintptr_t) typ;

    std::string obj_private_name;
    std::string typ_private_name;

    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        try {
            obj_private_name = custom_random_string(obj_id, attr_name, ::AllData::type_need_call[typ_id]);
            typ_private_name = custom_random_string(typ_id, attr_name, ::AllData::type_need_call[typ_id]);
        } catch (RestorePythonException& e) {
            e.restore();
            return NULL;
        }
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
        typ_private_name = default_random_string(typ_id, attr_name);
    }

    if (::AllData::all_object_attr.find(typ_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_AttributeError, "type not found");
        return NULL;
    }
    if (::AllData::all_object_attr[typ_id].find(obj_id) == ::AllData::all_object_attr[typ_id].end()) {
        ::AllData::all_object_attr[typ_id][obj_id] = {};
    }
    if (::AllData::all_object_mutex.find(typ_id) == ::AllData::all_object_mutex.end()) {
        ::AllData::all_object_mutex[typ_id] = {};
    }
    if (::AllData::all_object_mutex[typ_id].find(obj_id) == ::AllData::all_object_mutex[typ_id].end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_object_mutex[typ_id][obj_id] = lock;
    }
    if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_type_mutex[typ_id] = lock;
    }

    if (::AllData::type_attr_dict.find(typ_id) != ::AllData::type_attr_dict.end()) {
        PyObject* python_obj = NULL;
        PyObject* python_result;
        {
            std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
            if (::AllData::type_attr_dict[typ_id].find(typ_private_name) != ::AllData::type_attr_dict[typ_id].end()) {
                python_obj = ::AllData::type_attr_dict[typ_id][typ_private_name];
            }
        }
        if (python_obj && PyObject_HasAttrString(python_obj, "__get__") && PyObject_HasAttrString(python_obj, "__set__")) {
            python_result = PyObject_CallMethod(python_obj, "__get__", "(OO)", obj, typ);
            return python_result;
        }
        if (::AllData::all_object_attr[typ_id][obj_id].find(obj_private_name) != ::AllData::all_object_attr[typ_id][obj_id].end()) {
            std::shared_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[typ_id][obj_id]);
            return ::AllData::all_object_attr[typ_id][obj_id][obj_private_name];
        }
        if (python_obj && PyObject_HasAttrString(python_obj, "__get__") && !PyObject_HasAttrString(python_obj, "__set__")) {
            python_result = PyObject_CallMethod(python_obj, "__get__", "(OO)", obj, typ);
            return python_result;
        }
        if (!python_obj) {
            std::string type_name = PyUnicode_AsUTF8(PyObject_GetAttrString(typ, "__name__"));
            std::string exception_information = "'" + type_name + "' object has no attribute '" + attr_name + "'";
            PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
            PyObject *exc_typ, *exc_value, *traceback;
            PyErr_Fetch(&exc_typ, &exc_value, &traceback);
            PyObject_SetAttrString(exc_value, "obj", obj);
            PyObject_SetAttrString(exc_value, "name", PyUnicode_FromString(attr_name.c_str()));
            PyErr_Restore(exc_typ, exc_value, traceback);
        }
        return python_obj;
    } else {
        if (::AllData::all_object_attr[typ_id][obj_id].find(obj_private_name) != ::AllData::all_object_attr[typ_id][obj_id].end()) {
            std::shared_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[typ_id][obj_id]);
            return ::AllData::all_object_attr[typ_id][obj_id][obj_private_name];
        }
    }

    std::string type_name = PyUnicode_AsUTF8(PyObject_GetAttrString(typ, "__name__"));
    std::string exception_information = "'" + type_name + "' object has no attribute '" + attr_name + "'";
    PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
    PyObject *exc_typ, *exc_value, *traceback;
    PyErr_Fetch(&exc_typ, &exc_value, &traceback);
    PyObject_SetAttrString(exc_value, "obj", obj);
    PyObject_SetAttrString(exc_value, "name", PyUnicode_FromString(attr_name.c_str()));
    PyErr_Restore(exc_typ, exc_value, traceback);
    return NULL;
}

static PyObject*
type_getattr(PyObject* typ, std::string attr_name)
{
    long long typ_id = (long long) (uintptr_t) typ;
    std::string typ_private_name;
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        try {
            typ_private_name = custom_random_string(typ_id, attr_name, ::AllData::type_need_call[typ_id]);
        } catch (RestorePythonException& e) {
            e.restore();
            return NULL;
        }
    } else {
        typ_private_name = default_random_string(typ_id, attr_name);
    }
    if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_type_mutex[typ_id] = lock;
    }

    if (::AllData::type_attr_dict.find(typ_id) != ::AllData::type_attr_dict.end()) {
        PyObject* python_obj;
        {
            std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
            if (::AllData::type_attr_dict[typ_id].find(typ_private_name) == ::AllData::type_attr_dict[typ_id].end()){
                std::string type_name = ((PyTypeObject*)typ)->tp_name;
                std::string exception_information = "AttributeError: type '" + type_name + "' object has no attribute '" + attr_name + "'";
                PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
                return NULL;
            }
            python_obj = ::AllData::type_attr_dict[typ_id][typ_private_name];
        }
        if (!python_obj) {
            PyErr_SetString(PyExc_AttributeError, "attribute is NULL");
            return NULL;
        }
        if (PyObject_HasAttrString(python_obj, "__get__")) {
            PyObject* python_result = PyObject_CallMethod(python_obj, "__get__", "(OO)", Py_None, typ);
            return python_result;
        }
        Py_INCREF(python_obj);
        return python_obj;
    } else {
        ::AllData::type_attr_dict[typ_id] = {};
        std::string type_name = ((PyTypeObject*)typ)->tp_name;
        std::string exception_information = "type '" + type_name + "' has no attribute '" + attr_name + "'";
        PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
        return NULL;
    }
}

static int
id_setattr(std::string attr_name, PyObject* obj, PyObject* typ, PyObject* value)
{
    long long obj_id, typ_id;
    obj_id = (long long) (uintptr_t) obj;
    typ_id = (long long) (uintptr_t) typ;

    std::string obj_private_name;
    std::string typ_private_name;
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        try {
            obj_private_name = custom_random_string(obj_id, attr_name, ::AllData::type_need_call[typ_id]);
            typ_private_name = custom_random_string(typ_id, attr_name, ::AllData::type_need_call[typ_id]);
        } catch (RestorePythonException& e) {
            e.restore();
            return -1;
        }
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
        typ_private_name = default_random_string(typ_id, attr_name);
    }

    if (::AllData::all_object_attr.find(typ_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_AttributeError, "type not found");
        return -1;
    }
    if (::AllData::all_object_attr[typ_id].find(obj_id) == ::AllData::all_object_attr[typ_id].end()) {
        ::AllData::all_object_attr[typ_id][obj_id] = {};
    }
    if (::AllData::all_object_mutex.find(typ_id) == ::AllData::all_object_mutex.end()) {
        ::AllData::all_object_mutex[typ_id] = {};
    }
    if (::AllData::all_object_mutex[typ_id].find(obj_id) == ::AllData::all_object_mutex[typ_id].end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_object_mutex[typ_id][obj_id] = lock;
    }
    if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_type_mutex[typ_id] = lock;
    }

    // first: find attribute on type to find "__set__"
    if (::AllData::type_attr_dict.find(typ_id) != ::AllData::type_attr_dict.end()) {
        PyObject* python_obj;
        {
            std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
            python_obj = ::AllData::type_attr_dict[typ_id][typ_private_name];
        }
        if (python_obj && PyObject_HasAttrString(python_obj, "__set__")) {
            if (!PyObject_CallMethod(python_obj, "__set__", "(OO)", obj, value)) {
                return -1;
            }
            return 0;
        }
    }
    // second: set attribute on obj
    Py_INCREF(value);
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[typ_id][obj_id]);
        if (::AllData::all_object_attr[typ_id][obj_id].find(obj_private_name) != ::AllData::all_object_attr[typ_id][obj_id].end()) {
            Py_DECREF(::AllData::all_object_attr[typ_id][obj_id][obj_private_name]);
        }
        ::AllData::all_object_attr[typ_id][obj_id][obj_private_name] = value;
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
    long long typ_id = (long long) (uintptr_t) typ;
    std::string typ_private_name;
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        try {
            typ_private_name = custom_random_string(typ_id, attr_name, ::AllData::type_need_call[typ_id]);
        } catch (RestorePythonException& e) {
            e.restore();
            return -1;
        }
    } else {
        typ_private_name = default_random_string(typ_id, attr_name);
    }

    if (::AllData::type_attr_dict.find(typ_id) == ::AllData::type_attr_dict.end()) {
        ::AllData::type_attr_dict[typ_id] = {};
    }
    if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_type_mutex[typ_id] = lock;
    }
    Py_INCREF(value);
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
        if (::AllData::type_attr_dict[typ_id].find(typ_private_name) != ::AllData::type_attr_dict[typ_id].end()) {
            Py_DECREF(::AllData::type_attr_dict[typ_id][typ_private_name]);
        }
        ::AllData::type_attr_dict[typ_id][typ_private_name] = value;
    }
    return 0;
}

static int
id_delattr(std::string attr_name, PyObject* obj, PyObject* typ)
{
    long long obj_id, typ_id;
    obj_id = (long long) (uintptr_t) obj;
    typ_id = (long long) (uintptr_t) typ;

    std::string obj_private_name;
    std::string typ_private_name;
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        try {
            obj_private_name = custom_random_string(obj_id, attr_name, ::AllData::type_need_call[typ_id]);
            typ_private_name = custom_random_string(typ_id, attr_name, ::AllData::type_need_call[typ_id]);
        } catch (RestorePythonException& e) {
            e.restore();
            return -1;
        }
    } else {
        obj_private_name = default_random_string(obj_id, attr_name);
        typ_private_name = default_random_string(typ_id, attr_name);
    }

    if (::AllData::all_object_attr.find(typ_id) == ::AllData::all_object_attr.end()) {
        PyErr_SetString(PyExc_AttributeError, "type not found");
        return -1;
    }
    if (::AllData::all_object_attr[typ_id].find(obj_id) == ::AllData::all_object_attr[typ_id].end()) {
        ::AllData::all_object_attr[typ_id][obj_id] = {};
    }
    if (::AllData::all_object_mutex.find(typ_id) == ::AllData::all_object_mutex.end()) {
        ::AllData::all_object_mutex[typ_id] = {};
    }
    if (::AllData::all_object_mutex[typ_id].find(obj_id) == ::AllData::all_object_mutex[typ_id].end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_object_mutex[typ_id][obj_id] = lock;
    }
    if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_type_mutex[typ_id] = lock;
    }
    // first: find attribute on type to find "__delete__"
    if (::AllData::type_attr_dict.find(typ_id) != ::AllData::type_attr_dict.end()) {
        PyObject* python_obj;
        {
            std::shared_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
            python_obj = ::AllData::type_attr_dict[typ_id][typ_private_name];
        }
        if (PyObject_HasAttrString(python_obj, "__delete__")) {
            if (!PyObject_CallMethod(python_obj, "__delete__", "(O)", obj)) {
                return -1;
            }
            return 0;
        }
    }
    // second: delete attribute on obj
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_object_mutex[typ_id][obj_id]);
        if (::AllData::all_object_attr[typ_id][obj_id].find(obj_private_name) == ::AllData::all_object_attr[typ_id][obj_id].end()) {
            lock.release();
            std::string type_name = PyUnicode_AsUTF8(PyObject_GetAttrString(typ, "__name__"));
            std::string exception_information = "'" + type_name + "' object has no attribute '" + attr_name + "'";
            PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
            PyObject *exc_typ, *exc_value, *traceback;
            PyErr_Fetch(&exc_typ, &exc_value, &traceback);
            PyObject_SetAttrString(exc_value, "obj", obj);
            PyObject_SetAttrString(exc_value, "name", PyUnicode_FromString(attr_name.c_str()));
            PyErr_Restore(exc_typ, exc_value, traceback);
            return -1;
        }
        PyObject* delete_obj = ::AllData::all_object_attr[typ_id][obj_id][obj_private_name];
        ::AllData::all_object_attr[typ_id][obj_id].erase(obj_private_name);
        Py_XDECREF(delete_obj);
    }
    return 0;
}

static int
type_delattr(PyObject* typ, std::string attr_name)
{
    long long typ_id = (long long) (uintptr_t) typ;

    std::string typ_private_name;
    if (::AllData::type_need_call.find(typ_id) != ::AllData::type_need_call.end()) {
        try {
            typ_private_name = custom_random_string(typ_id, attr_name, ::AllData::type_need_call[typ_id]);
        } catch (RestorePythonException& e) {
            e.restore();
            return -1;
        }
    } else {
        typ_private_name = default_random_string(typ_id, attr_name);
    }

    if (::AllData::type_attr_dict.find(typ_id) == ::AllData::type_attr_dict.end()) {
        ::AllData::type_attr_dict[typ_id] = {};
    }
        if (::AllData::all_type_mutex.find(typ_id) == ::AllData::all_type_mutex.end()) {
        std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
        ::AllData::all_type_mutex[typ_id] = lock;
    }
    {
        std::unique_lock<std::shared_mutex> lock(*::AllData::all_type_mutex[typ_id]);
        if (::AllData::type_attr_dict[typ_id].find(typ_private_name) == ::AllData::type_attr_dict[typ_id].end()){
            lock.release();
            std::string type_name = PyUnicode_AsUTF8(PyObject_GetAttrString(typ, "__name__"));
            std::string exception_information = "type '" + type_name + "' has no attribute '" + attr_name + "'";
            PyErr_SetString(PyExc_AttributeError, exception_information.c_str());
            PyObject *exc_typ, *exc_value, *traceback;
            PyErr_Fetch(&exc_typ, &exc_value, &traceback);
            PyObject_SetAttrString(exc_value, "name", PyUnicode_FromString(attr_name.c_str()));
            PyObject_SetAttrString(exc_value, "obj", typ);
            PyErr_Restore(exc_typ, exc_value, traceback);
            return -1;
        }
        Py_DECREF(::AllData::type_attr_dict[typ_id][typ_private_name]);
        ::AllData::type_attr_dict[typ_id].erase(typ_private_name);
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
    if (doc) {
        Py_INCREF(doc);
    } else {
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
    if (module) {
        Py_INCREF(module);
    } else {
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
    if (name) {
        Py_INCREF(name);
    } else {
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
    if (qualname) {
        Py_INCREF(qualname);
    } else {
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
    if (annotate) {
        Py_INCREF(annotate);
    } else {
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
    if (type_params) {
        Py_INCREF(type_params);
    } else {
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
    if (res) {
        Py_INCREF(res);
    } else {
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

static PyObject*
PrivateAttr_tp_getattro(PyObject* self, PyObject* name)
{
    long long type_id = (long long)(uintptr_t)Py_TYPE(self);
    if (::AllData::all_function_creator.find(type_id) == ::AllData::all_function_creator.end()) {
        PyErr_SetString(PyExc_SystemError, "type_id not found");
        return NULL;
    }
    std::shared_ptr<FunctionCreator> fc = ::AllData::all_function_creator[type_id];
    PyObject* result = fc->getattro(self, name);
    if (!result && PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        result = fc->getattr(self, name);
    }
    return result;
}

static int
PrivateAttr_tp_setattro(PyObject* self, PyObject* name, PyObject* value)
{
    long long type_id = (long long)(uintptr_t)Py_TYPE(self);
    if (::AllData::all_function_creator.find(type_id) == ::AllData::all_function_creator.end()) {
        PyErr_SetString(PyExc_SystemError, "type_id not found");
        return 1;
    }
    std::shared_ptr<FunctionCreator> fc = ::AllData::all_function_creator[type_id];
    if (!value) {
        return fc->delattr(self, name);
    }
    return fc->setattro(self, name, value);
}

static void
PrivateAttr_tp_dealloc(PyObject* self)
{
    long long type_id = (long long)(uintptr_t)Py_TYPE(self);
    if (::AllData::all_function_creator.find(type_id) != ::AllData::all_function_creator.end()) {
        std::shared_ptr<FunctionCreator> fc = ::AllData::all_function_creator[type_id];
        fc->del(self);
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
    long long type_id = reinterpret_cast<long long>(&PrivateAttrType);
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
    long long type_id = reinterpret_cast<long long>(&PrivateAttrType);
    name2 = std::to_string(type_id) + "_" + name1;
    std::string name1hash, name2hash;
    picosha2::hash256_hex_string(name1, name1hash);
    picosha2::hash256_hex_string(name2, name2hash);
    return TwoStringTuple(name1hash, name2hash);
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
analyse_all_code(PyObject* obj, std::vector<PyCodeObject*>& list, std::unordered_set<long long>& _seen)
{
    long long obj_id = (long long)(uintptr_t)obj;
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

static PyObject*
PrivateAttrType_new(PyTypeObject* type, PyObject* args, PyObject* kwds) 
{
    static const char* invalid_name[] = {"__private_attrs__", "__slots__", "__getattribute__", "__getattr__",
        "__setattr__", "__delattr__", "__name__", "__module__", "__doc__", "__getstate__", "__setstate__", NULL};
#if PY_VERSION_HEX < 0x030B0000
    static char* kwlist[] = {"name", "bases", "attrs", "private_func", NULL};
#else
    static const char* kwlist[] = {"name", "bases", "attrs", "private_func", NULL};
#endif

    PyObject* name;
    PyObject* bases;
    PyObject* attrs;
    PyObject* private_func = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO|O", kwlist,
        &name, &bases, &attrs, &private_func)) {
        return NULL;
    }

    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, "name must be a string");
        return NULL;
    }

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

    std::vector<std::string> private_attrs_vector_string;

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

        std::string attr_str = attr_cstr;

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
        private_attrs_vector_string.push_back(attr_str);
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
        for (const auto& attr_str : private_attrs_vector_string) {
            for (Py_ssize_t j = 0; j < slot_len; j++) {
                PyObject* slot = PySequence_Fast_GET_ITEM(slot_seq, j);
                if (PyUnicode_Check(slot)) {
                    const char* slot_cstr = PyUnicode_AsUTF8(slot);
                    if (slot_cstr && attr_str == slot_cstr) {
                        std::string error_msg = "attribute '" + attr_str + "' is already in '__slots__'";
                        PyErr_SetString(PyExc_TypeError, error_msg.c_str());
                        Py_DECREF(slot_seq);
                        Py_DECREF(attrs_copy);
                        Py_DECREF(new_hash_private_attrs);
                        return NULL;
                    }
                }
            }
        }
        Py_DECREF(slot_seq);
    }

    PyObject* type_args = PyTuple_Pack(3, name, bases, attrs_copy);
    PyObject* new_type = PyType_Type.tp_new(type, type_args, NULL);
    Py_DECREF(type_args);

    if (!new_type) {
        Py_DECREF(attrs_copy);
        Py_DECREF(new_hash_private_attrs);
        return NULL;
    }

    PyTypeObject* type_instance = (PyTypeObject*)new_type;

    type_instance->tp_getattro = PrivateAttr_tp_getattro;
    type_instance->tp_setattro = PrivateAttr_tp_setattro;
    type_instance->tp_dealloc = PrivateAttr_tp_dealloc;
    std::shared_ptr<FunctionCreator> creator = std::make_shared<FunctionCreator>(type_instance);
    long long type_id = (long long)(uintptr_t)(type_instance);
    ::AllData::type_attr_dict[type_id] = {};
    Py_ssize_t pos = 0;

    for (PyObject *key, *value; PyDict_Next(attrs_copy, &pos, &key, &value);) {
        if (!PyUnicode_Check(key)) {
            continue;
        }

        std::string key_str = PyUnicode_AsUTF8(key);
        if (std::find(private_attrs_vector_string.begin(),
                private_attrs_vector_string.end(), key_str) != private_attrs_vector_string.end()) {
            std::string final_key;

            if (private_func) {
                try {
                    final_key = custom_random_string(type_id, key_str, private_func);
                } catch (RestorePythonException& e) {
                    e.restore();
                    Py_DECREF(attrs_copy);
                    Py_DECREF(new_hash_private_attrs);
                    Py_DECREF(new_type);
                    return NULL;
                }
            } else {
                final_key = default_random_string(type_id, key_str);
            }

            PyObject* need_value;
            if (PyObject_TypeCheck(value, &PrivateWrapType)) {
                need_value = ((PrivateWrapObject*)value)->result;
            } else {
                need_value = value;
            }

            Py_INCREF(need_value);
            ::AllData::type_attr_dict[type_id][final_key] = need_value;

            PyDict_DelItem(type_instance->tp_dict, key);
        } else {
            if (PyObject_TypeCheck(value, &PrivateWrapType)) {
                PyObject* need_value = ((PrivateWrapObject*)value)->result;
                PyDict_SetItem(type_instance->tp_dict, key, need_value);
            }
        }
    }

    ::AllData::all_function_creator[type_id] = creator;
    
    ::AllData::type_allowed_code[type_id] = {};
    ::AllData::all_type_attr_set[type_id] = private_attrs_set;
    ::AllData::all_object_mutex[type_id] = {};
    ::AllData::all_type_mutex[type_id] = std::make_shared<std::shared_mutex>();
    ::AllData::all_object_attr[type_id] = {};

    if (private_func) {
        ::AllData::type_need_call[type_id] = private_func;
        Py_INCREF(private_func);
    }

    {
        PyObject* original_key;
        Py_ssize_t original_pos = 0;
        PyObject* original_value;
        while (PyDict_Next(attrs_copy, &original_pos, &original_key, &original_value)) {
            std::unordered_set<long long> set;
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
    long long typ_id = (long long)(uintptr_t)(cls);
    std::string name_str = PyUnicode_AsUTF8(name);
    PyCodeObject* now_code = get_now_code();
    TwoStringTuple name_hash_set = get_string_hash_tuple2(name_str);
    if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()) {
        if (::AllData::all_type_attr_set[typ_id].find(name_hash_set) != ::AllData::all_type_attr_set[typ_id].end()) {
            if (!now_code || (!is_class_code(typ_id, now_code) && !is_subclass_code((PyTypeObject*)cls, now_code))) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return NULL;
            }
            return type_getattr(cls, name_str);
        }
    }
    PyObject* mro = ((PyTypeObject*)cls)->tp_mro;
    for (int i=1; i < PyTuple_GET_SIZE(mro); i++) {
        PyTypeObject* parent_type = (PyTypeObject*)PyTuple_GET_ITEM(mro, i);
        long long parent_id = (long long)(uintptr_t)parent_type;
        if (::AllData::all_type_attr_set.find(parent_id) != ::AllData::all_type_attr_set.end()) {
            if (::AllData::all_type_attr_set[parent_id].find(name_hash_set) != ::AllData::all_type_attr_set[parent_id].end()) {
                if (!now_code || (!is_class_code(typ_id, now_code) && !is_subclass_code((PyTypeObject*)cls, now_code))) {
                    PyErr_SetString(PyExc_AttributeError, "private attribute");
                    return NULL;
                }
                return type_getattr(cls, name_str);
            }
        }
    }
    return PyType_Type.tp_getattro(cls, name);
}

static int
PrivateAttrType_setattr(PyObject* cls, PyObject* name, PyObject* value)
{
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "cls must be a type");
        return -1;
    }
    long long typ_id = (long long)(uintptr_t)(cls);
    std::string name_str = PyUnicode_AsUTF8(name);
    PyCodeObject* now_code = get_now_code();
    TwoStringTuple name_hash_set = get_string_hash_tuple2(name_str);
    if (::AllData::all_type_attr_set.find(typ_id) != ::AllData::all_type_attr_set.end()) {
        if (::AllData::all_type_attr_set[typ_id].find(name_hash_set) != ::AllData::all_type_attr_set[typ_id].end()) {
            if (!now_code || (!is_class_code(typ_id, now_code) && !is_subclass_code((PyTypeObject*)cls, now_code))) {
                PyErr_SetString(PyExc_AttributeError, "private attribute");
                return -1;
            }
            return type_setattr(cls, name_str, value);
        }
    }
    PyObject* mro = ((PyTypeObject*)cls)->tp_mro;
    for (int i=1; i < PyTuple_GET_SIZE(mro); i++) {
        PyTypeObject* parent_type = (PyTypeObject*)PyTuple_GET_ITEM(mro, i);
        long long parent_id = (long long)(uintptr_t)parent_type;
        if (::AllData::all_type_attr_set.find(parent_id) != ::AllData::all_type_attr_set.end()) {
            if (::AllData::all_type_attr_set[parent_id].find(name_hash_set) != ::AllData::all_type_attr_set[parent_id].end()) {
                if (!now_code || (!is_class_code(typ_id, now_code) && !is_subclass_code((PyTypeObject*)cls, now_code))) {
                    PyErr_SetString(PyExc_AttributeError, "private attribute");
                    return -1;
                }
                return type_setattr(cls, name_str, value);
            }
        }
    }
    return PyType_Type.tp_setattro(cls, name, value);
}

static void
PrivateAttrType_del(PyObject* cls)
{
    long long typ_id = (long long)(uintptr_t) cls;
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
    ::AllData::all_type_mutex.erase(typ_id);
    clear_obj(typ_id);
    PrivateAttrType.tp_free(cls);
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
    ((PyTypeObject*)base_type)->tp_name = "PrivateAttrBase";
    return base_type;
}

typedef struct PrivateModule{
    PyObject_HEAD
}PrivateModule;

static PyObject*
PrivateModule_get_PrivateWrapProxy(PyObject* /*self*/, void* /*closure*/)
{
    return (PyObject*)&PrivateWrapProxyType;
}

// type PrivateAttrType
static PyObject*
PrivateModule_get_PrivateAttrType(PyObject* /*self*/, void* /*closure*/)
{
    return (PyObject*)&PrivateAttrType;
}

static PyObject*
PrivateModule_get_PrivateAttrBase(PyObject* /*self*/, void* /*closure*/)
{
    static PyObject* PrivateAttrBase = create_private_attr_base_simple();
    return PrivateAttrBase;
}

static PyObject*
PrivateModule_dir(PyObject* self)
{
    PyObject* parent_dir = PyObject_CallMethod((PyObject*)&PyModule_Type, "__dir__", "O", self);
    if (!parent_dir) return NULL;
    PyObject* attr_list = PyList_New(0);
    if (!attr_list) return NULL;
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
