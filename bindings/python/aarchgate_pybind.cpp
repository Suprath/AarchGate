#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include "apex/apex_c_api.h"
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

namespace py = pybind11;

class PyApexEngine {
public:
    PyApexEngine() {
        handle_ = apex_create();
        if (!handle_) {
            throw std::runtime_error("Failed to create Apex Engine");
        }
    }

    ~PyApexEngine() {
        if (handle_) {
            apex_destroy(handle_);
        }
    }

    void register_schema(const std::string& name, py::list fields_list, size_t stride) {
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

        if (apex_register_schema(handle_, name.c_str(), fields.data(), fields.size(), stride) != 0) {
            throw std::runtime_error("Failed to register schema");
        }
    }

    void set_logic(const std::string& schema_name, size_t ir_root_ptr, int mode) {
        if (apex_set_logic(handle_, schema_name.c_str(), reinterpret_cast<void*>(ir_root_ptr), mode) != 0) {
            throw std::runtime_error("Failed to set logic");
        }
    }

    uint64_t execute(py::array_t<uint8_t, py::array::c_style | py::array::forcecast> data, size_t count) {
        py::buffer_info info = data.request();
        void* ptr = info.ptr;
        
        // No need for alignment on the data buffer as gather_field uses memcpy
        uint64_t result = apex_execute(handle_, ptr, count);

        if (result == (uint64_t)-1) {
            throw std::runtime_error("Execution failed");
        }
        return result;
    }

private:
    apex_engine_h handle_;
};

PYBIND11_MODULE(apex_python, m) {
    m.doc() = "Apex Engine Python Bindings";

    m.def("create_universal_test_logic", []() {
        return reinterpret_cast<size_t>(apex_create_universal_test_logic());
    });
    
    m.def("create_simple_logic", []() {
        return reinterpret_cast<size_t>(apex_create_simple_logic());
    });

    py::class_<PyApexEngine>(m, "ApexEngine")
        .def(py::init<>())
        .def("register_schema", &PyApexEngine::register_schema)
        .def("set_logic", &PyApexEngine::set_logic)
        .def("execute", &PyApexEngine::execute);
}
