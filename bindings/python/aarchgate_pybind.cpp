#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include "apex/apex_c_api.h"
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <cstdint>

namespace py = pybind11;

// Opaque wrapper for the engine handle
class PyApexEngine {
public:
    PyApexEngine() {
        handle_ = apex_create();
    }

    ~PyApexEngine() {
        if (handle_) {
            apex_destroy(handle_);
        }
    }
    
    void* get_handle() const { return (void*)handle_; }

private:
    apex_engine_h handle_;
};

// Global functions to avoid class-method registration issues
static void py_register_schema(PyApexEngine& engine, const char* name, py::list fields_list, size_t stride) {
    std::vector<std::string> names;
    names.reserve(py::len(fields_list));
    std::vector<apex_field_descriptor_t> fields;
    fields.reserve(py::len(fields_list));
    
    for (auto item : fields_list) {
        auto tuple = item.cast<py::tuple>();
        names.push_back(tuple[0].cast<std::string>());
        fields.push_back({
            names.back().c_str(),
            tuple[1].cast<size_t>(),
            tuple[2].cast<size_t>(),
            tuple[3].cast<int>()
        });
    }

    if (apex_register_schema(engine.get_handle(), name, fields.data(), fields.size(), stride) != 0) {
        throw std::runtime_error("Failed to register schema");
    }
}

static void py_set_logic(PyApexEngine& engine, const char* schema_name, py::capsule ir_root, int mode) {
    if (apex_set_logic(engine.get_handle(), schema_name, ir_root.get_pointer(), mode) != 0) {
        throw std::runtime_error("Failed to set logic");
    }
}

static uint64_t py_execute(PyApexEngine& engine, py::buffer b, size_t count) {
    py::buffer_info info = b.request();
    return apex_execute(engine.get_handle(), info.ptr, count);
}

static uint64_t py_execute_parallel(PyApexEngine& engine, py::buffer b, size_t count, int num_threads) {
    py::buffer_info info = b.request();
    return apex_execute_parallel(engine.get_handle(), info.ptr, count, num_threads);
}

// Builder functions
static py::capsule py_builder_load(const char* name) {
    return py::capsule(apex_builder_load(name));
}

static py::capsule py_builder_const(int64_t value) {
    return py::capsule(apex_builder_const(value));
}

static py::capsule py_builder_add(py::capsule a, py::capsule b) {
    return py::capsule(apex_builder_add(a.get_pointer(), b.get_pointer()));
}

static py::capsule py_builder_gt(py::capsule a, py::capsule b) {
    return py::capsule(apex_builder_gt(a.get_pointer(), b.get_pointer()));
}

static py::capsule py_builder_ge(py::capsule a, py::capsule b) {
    return py::capsule(apex_builder_ge(a.get_pointer(), b.get_pointer()));
}

static py::capsule py_builder_lt(py::capsule a, py::capsule b) {
    return py::capsule(apex_builder_lt(a.get_pointer(), b.get_pointer()));
}

static py::capsule py_builder_and(py::capsule a, py::capsule b) {
    return py::capsule(apex_builder_and(a.get_pointer(), b.get_pointer()));
}

static py::capsule py_builder_select(py::capsule cond, py::capsule a, py::capsule b) {
    return py::capsule(apex_builder_select(cond.get_pointer(), a.get_pointer(), b.get_pointer()));
}

static py::capsule py_builder_not(py::capsule a) {
    return py::capsule(apex_builder_not(a.get_pointer()));
}

static void py_builder_set_weight(py::capsule node, int64_t weight) {
    apex_builder_set_weight(node.get_pointer(), weight);
}

static py::capsule py_builder_sum(py::list operands) {
    std::vector<void*> nodes;
    nodes.reserve(operands.size());
    for (auto item : operands) {
        try {
            nodes.push_back(item.cast<py::capsule>().get_pointer());
        } catch (...) {
            // Skip invalid operands instead of crashing
        }
    }
    return py::capsule(apex_builder_sum(nodes.data(), nodes.size()));
}

PYBIND11_MODULE(aarchgate_python, m) {
    // Register the class as an opaque type
    py::class_<PyApexEngine>(m, "ApexEngine")
        .def(py::init<>());

    // Register all operations as module-level functions
    m.def("register_schema", &py_register_schema);
    m.def("set_logic", &py_set_logic);
    m.def("execute", &py_execute);
    m.def("execute_parallel", &py_execute_parallel);

    m.def("builder_Load", &py_builder_load);
    m.def("builder_Const", &py_builder_const);
    m.def("builder_Add", &py_builder_add);
    m.def("builder_GT", &py_builder_gt);
    m.def("builder_GE", &py_builder_ge);
    m.def("builder_LT", &py_builder_lt);
    m.def("builder_AND", &py_builder_and);
    m.def("builder_Select", &py_builder_select);
    m.def("builder_Sum", &py_builder_sum);
    m.def("builder_Not", &py_builder_not);
    m.def("builder_SetWeight", &py_builder_set_weight);
}
