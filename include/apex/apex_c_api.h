#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for the Apex Engine
typedef void* apex_engine_h;

// Define field descriptor for C API
typedef struct {
    const char* name;
    size_t offset;
    size_t bit_width;
    int data_type; // Maps to apex::core::DataType enum: 0=UINT8, 1=UINT16, 2=UINT32, 3=UINT64, 4=INT8, 5=INT16, 6=INT32, 7=INT64
} apex_field_descriptor_t;

// Execution modes mapping to apex::ExecutionMode
#define APEX_EXEC_MODE_BIT_SLICED 0
#define APEX_EXEC_MODE_SCALAR 1

// Create a new engine instance
// Returns handle on success, NULL on failure
__attribute__((visibility("default"))) apex_engine_h apex_create(void);

// Destroy an engine instance
__attribute__((visibility("default"))) void apex_destroy(apex_engine_h handle);

// Register a schema
// Returns 0 on success, non-zero on failure
__attribute__((visibility("default"))) int apex_register_schema(
    apex_engine_h handle, 
    const char* schema_name, 
    const apex_field_descriptor_t* fields, 
    size_t num_fields, 
    size_t stride);

// Set the expression logic for the engine
// Returns 0 on success, non-zero on failure
__attribute__((visibility("default"))) int apex_set_logic(
    apex_engine_h handle, 
    const char* schema_name, 
    void* ir_root_ptr, 
    int mode);

// Execute the registered logic across data
// Returns the total number of matches, or (uint64_t)-1 on failure
__attribute__((visibility("default"))) uint64_t apex_execute(
    apex_engine_h handle, 
    const void* data_ptr, 
    size_t count);

// Helper for multi-language verification tests
// Returns ir_root_ptr for: (Field0 + Field1) > Field2
__attribute__((visibility("default"))) void* apex_create_universal_test_logic(void);

// Returns ir_root_ptr for: Field0 > 10
__attribute__((visibility("default"))) void* apex_create_simple_logic(void);

// IR Builder functions for external language bindings
__attribute__((visibility("default"))) void* apex_builder_load(const char* name);
__attribute__((visibility("default"))) void* apex_builder_const(int64_t value);
__attribute__((visibility("default"))) void* apex_builder_add(void* a, void* b);
__attribute__((visibility("default"))) void* apex_builder_gt(void* a, void* b);
__attribute__((visibility("default"))) void* apex_builder_ge(void* a, void* b);
__attribute__((visibility("default"))) void* apex_builder_lt(void* a, void* b);
__attribute__((visibility("default"))) void* apex_builder_and(void* a, void* b);
__attribute__((visibility("default"))) void* apex_builder_select(void* cond, void* a, void* b);
__attribute__((visibility("default"))) void* apex_builder_sum(void** operands, size_t count);

#ifdef __cplusplus
}
#endif
