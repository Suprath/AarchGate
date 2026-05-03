# AarchLogic SDK Reference

## Complete API Contract for All Language Bindings

---

## Table of Contents

1. [C API (Foundation)](#c-api-foundation)
2. [C++ Wrapper API](#c-wrapper-api)
3. [Python Bindings](#python-bindings)
4. [Java Bindings](#java-bindings)
5. [Data Types & Enumerations](#data-types--enumerations)
6. [Error Codes](#error-codes)
7. [Memory Management](#memory-management)
8. [Execution Modes](#execution-modes)
9. [Schema Definition](#schema-definition)
10. [Examples](#examples)

---

## C API (Foundation)

### Header File
```c
#include <apex/apex_c_api.h>
```

### Engine Lifecycle

#### `apex_create()`
```c
apex_engine_h apex_create(void);
```

**Description**: Create a new AarchLogic engine instance.

**Parameters**: None

**Returns**: 
- Opaque handle on success
- NULL on failure

**Example**:
```c
apex_engine_h engine = apex_create();
if (!engine) {
    fprintf(stderr, "Failed to create engine\n");
    return 1;
}
```

---

#### `apex_destroy()`
```c
void apex_destroy(apex_engine_h handle);
```

**Description**: Destroy engine instance and release all resources.

**Parameters**:
- `handle`: Engine handle from `apex_create()`

**Returns**: Void

**Notes**:
- Safe to call multiple times on same handle (no-op after first call)
- Must be called before process exit to prevent resource leaks

**Example**:
```c
apex_destroy(engine);
engine = NULL;  // Good practice
```

---

### Schema Registration

#### `apex_register_schema()`
```c
int apex_register_schema(
    apex_engine_h handle,
    const char* schema_name,
    const apex_field_descriptor_t* fields,
    size_t num_fields,
    size_t stride
);
```

**Description**: Register a data schema with field definitions.

**Parameters**:
- `handle`: Engine handle
- `schema_name`: Schema identifier (e.g., "trade", "packet")
- `fields`: Array of field descriptors
- `num_fields`: Number of fields
- `stride`: Size of one row in bytes (includes padding)

**Returns**:
- `0` on success
- Non-zero error code on failure

**Constraints**:
- `schema_name` must be unique within engine
- `stride >= (max_offset + max_bit_width / 8)`
- `num_fields <= 256`

**Example**:
```c
apex_field_descriptor_t fields[] = {
    {"price", 0, 64, APEX_UINT64},      // offset 0, 64 bits
    {"volume", 8, 32, APEX_UINT32},     // offset 8, 32 bits
    {"symbol", 12, 32, APEX_UINT32},    // offset 12, 32 bits (enum)
};

int status = apex_register_schema(
    engine, 
    "trade", 
    fields, 
    3, 
    16  // stride: 3 * 4 bytes + alignment padding
);

if (status != 0) {
    fprintf(stderr, "Registration failed: %d\n", status);
}
```

---

### Logic Compilation & Execution

#### `apex_set_logic()`
```c
int apex_set_logic(
    apex_engine_h handle,
    const char* schema_name,
    void* ir_root_ptr,
    int mode
);
```

**Description**: Set the expression logic to evaluate on data.

**Parameters**:
- `handle`: Engine handle
- `schema_name`: Schema to bind logic to
- `ir_root_ptr`: Intermediate representation (IR) of expression tree
- `mode`: Execution mode (APEX_EXEC_MODE_BIT_SLICED or APEX_EXEC_MODE_SCALAR)

**Returns**:
- `0` on success
- Non-zero error code on failure

**Notes**:
- `ir_root_ptr` must be valid IR node created by expr builder or helper
- Mode selection:
  - `APEX_EXEC_MODE_BIT_SLICED`: Default, high throughput, 64-row batches
  - `APEX_EXEC_MODE_SCALAR`: Low latency, single-row evaluation

**Example**:
```c
// Using helper: Field0 > 10
void* logic_ir = apex_create_simple_logic();

int status = apex_set_logic(
    engine,
    "trade",
    logic_ir,
    APEX_EXEC_MODE_BIT_SLICED
);

if (status != 0) {
    fprintf(stderr, "Failed to set logic\n");
}
```

---

#### `apex_execute()`
```c
uint64_t apex_execute(
    apex_engine_h handle,
    const void* data_ptr,
    size_t count
);
```

**Description**: Execute registered logic across data.

**Parameters**:
- `handle`: Engine handle
- `data_ptr`: Pointer to data buffer (row-major, matching schema stride)
- `count`: Number of rows to process

**Returns**:
- Total match count on success
- `(uint64_t)-1` (UINT64_MAX) on error

**Performance**:
- Throughput: ~1.3B rows/sec (4-thread on ARM64)
- Latency: Deterministic, ~100ns per 64-row batch
- Zero copies, no allocations

**Example**:
```c
uint8_t* data = malloc(128 * 1000000 * 16);  // 128M rows, stride=16
// ... fill data with trade records ...

uint64_t matches = apex_execute(engine, data, 128000000);

if (matches == (uint64_t)-1) {
    fprintf(stderr, "Execution failed\n");
} else {
    printf("Matched %llu rows\n", matches);
}

free(data);
```

---

### Helper Functions

#### `apex_create_universal_test_logic()`
```c
void* apex_create_universal_test_logic(void);
```

**Description**: Create IR for `(Field0 + Field1) > Field2`.

**Returns**: IR root pointer

**Use Case**: Testing, verification, benchmarking

**Example**:
```c
void* logic = apex_create_universal_test_logic();
apex_set_logic(engine, "test_schema", logic, APEX_EXEC_MODE_BIT_SLICED);
```

---

#### `apex_create_simple_logic()`
```c
void* apex_create_simple_logic(void);
```

**Description**: Create IR for `Field0 > 10`.

**Returns**: IR root pointer

**Use Case**: Simple threshold checking, quick validation

**Example**:
```c
void* logic = apex_create_simple_logic();
apex_set_logic(engine, "prices", logic, APEX_EXEC_MODE_BIT_SLICED);
uint64_t matches = apex_execute(engine, prices, count);
```

---

## C++ Wrapper API

### Header File
```cpp
#include <apex/AarchGate.hpp>
```

### Engine Wrapper

```cpp
class ApexEngine {
public:
    // Constructor: Creates engine, auto-detects P-cores on Apple Silicon
    ApexEngine();
    
    // Destructor: Destroys engine
    ~ApexEngine();
    
    // No copying allowed
    ApexEngine(const ApexEngine&) = delete;
    ApexEngine& operator=(const ApexEngine&) = delete;
    
    // Register schema with field vector
    void register_schema(
        const std::string& name,
        const std::vector<core::FieldDescriptor>& fields,
        size_t stride
    );
    
    // Set logic with execution mode
    void set_expression(
        const std::string& schema_name,
        ir::Node* expr_root,
        ExecutionMode mode = ExecutionMode::BIT_SLICED
    );
    
    // Concurrency Control
    void set_thread_count(int count);
    int get_thread_count() const;

    // Execute: Returns match count, throws on error
    uint64_t execute(const void* data_ptr, size_t count);
    uint64_t execute_parallel(const void* data_ptr, size_t count, int num_threads = -1);
};

} // namespace apex
```

### Usage Example

```cpp
#include <apex/AarchGate.hpp>
#include <vector>

int main() {
    try {
        // Create engine
        apex::Apex engine;
        
        // Register schema
        engine.register_schema("trade", {
            {"price", 0, 64, APEX_UINT64},
            {"volume", 8, 32, APEX_UINT32},
        }, 16);
        
        // Set logic
        void* logic = apex_create_simple_logic();
        engine.set_logic("trade", logic, APEX_EXEC_MODE_BIT_SLICED);
        
        // Load data and execute
        std::vector<uint8_t> data(128 * 1000000 * 16);
        // ... populate data ...
        
        uint64_t matches = engine.execute(data.data(), 128000000);
        std::cout << "Matched " << matches << " rows\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
```

---

## Python Bindings

### Module Import
```python
import aarchgate_python
# or
from aarchgate_python import *
```

### Engine Class

```python
class AarchGateEngine:
    """AarchLogic engine for Python."""
    
    def __init__(self):
        """Create a new engine instance."""
        pass
    
    def register_schema(
        self, 
        schema_name: str, 
        fields: List[Field], 
        stride: int
    ) -> None:
        """Register a data schema."""
        pass
    
    def set_logic(
        self,
        schema_name: str,
        ir_root: int,  # Opaque IR pointer from C++
        mode: int = BIT_SLICED
    ) -> None:
        """Set logic for schema."""
        pass
    
    def execute(
        self,
        data: np.ndarray,
        count: int
    ) -> int:
        """Execute logic, return match count."""
        pass
```

### Field Descriptor

```python
class Field:
    def __init__(
        self,
        name: str,
        offset: int,
        bit_width: int,
        data_type: int
    ):
        self.name = name
        self.offset = offset
        self.bit_width = bit_width
        self.data_type = data_type
```

### Enumerations

```python
# Execution Modes
BIT_SLICED = 0
SCALAR = 1

# Data Types
UINT8 = 0
UINT16 = 1
UINT32 = 2
UINT64 = 3
INT8 = 4
INT16 = 5
INT32 = 6
INT64 = 7
```

### Example

```python
import aarchgate_python as ag
import numpy as np

# Create engine
engine = ag.AarchGateEngine()

# Register schema
fields = [
    ag.Field("price", 0, 64, ag.UINT64),
    ag.Field("volume", 8, 32, ag.UINT32),
]
engine.register_schema("trade", fields, stride=16)

# Set logic
logic = ag.create_simple_logic()  # Field0 > 10
engine.set_logic("trade", logic, ag.BIT_SLICED)

# Create data (128M rows)
data = np.zeros((128_000_000,), dtype=np.uint8)
# ... populate data array ...

# Execute
matches = engine.execute(data, 128_000_000)
print(f"Matched {matches} rows")
```

---

## Java Bindings

### Class API

```java
package com.aarchgate.engine;

public class AarchGateEngine {
    /**
     * Create a new AarchLogic engine.
     */
    public AarchGateEngine();
    
    /**
     * Register a schema.
     */
    public void registerSchema(
        String schemaName,
        Field[] fields,
        int stride
    ) throws AarchGateException;
    
    /**
     * Set expression logic.
     */
    public void setLogic(
        String schemaName,
        long irRootPtr,  // Opaque pointer
        int mode
    ) throws AarchGateException;
    
    /**
     * Execute logic on data.
     */
    public long execute(
        ByteBuffer data,
        long count
    ) throws AarchGateException;
}
```

### Supporting Classes

```java
public class Field {
    public String name;
    public int offset;
    public int bitWidth;
    public DataType dataType;
    
    public Field(String name, int offset, int bitWidth, DataType type) {
        this.name = name;
        this.offset = offset;
        this.bitWidth = bitWidth;
        this.dataType = type;
    }
}

public enum DataType {
    UINT8(0), UINT16(1), UINT32(2), UINT64(3),
    INT8(4), INT16(5), INT32(6), INT64(7);
    
    private final int value;
    DataType(int v) { value = v; }
    public int getValue() { return value; }
}

public enum ExecutionMode {
    BIT_SLICED(0),
    SCALAR(1);
    
    private final int value;
    ExecutionMode(int v) { value = v; }
    public int getValue() { return value; }
}

public class AarchGateException extends Exception {
    public AarchGateException(String message) {
        super(message);
    }
}
```

### Example

```java
import java.nio.ByteBuffer;
import com.aarchgate.engine.*;

public class TradeAnalyzer {
    public static void main(String[] args) throws AarchGateException {
        // Create engine
        AarchGateEngine engine = new AarchGateEngine();
        
        // Register schema
        Field[] fields = {
            new Field("price", 0, 64, DataType.UINT64),
            new Field("volume", 8, 32, DataType.UINT32),
        };
        engine.registerSchema("trade", fields, 16);
        
        // Set logic
        long logicIR = createSimpleLogic();  // Helper
        engine.setLogic("trade", logicIR, ExecutionMode.BIT_SLICED.getValue());
        
        // Create buffer (128M rows × 16 bytes)
        ByteBuffer buffer = ByteBuffer.allocateDirect(128_000_000 * 16);
        // ... populate buffer ...
        
        // Execute
        long matches = engine.execute(buffer, 128_000_000);
        System.out.println("Matched " + matches + " rows");
    }
    
    // Native helper (linked via JNI)
    private static native long createSimpleLogic();
}
```

---

## Data Types & Enumerations

### DataType (Numeric)

```c
#define APEX_UINT8    0
#define APEX_UINT16   1
#define APEX_UINT32   2
#define APEX_UINT64   3
#define APEX_INT8     4
#define APEX_INT16    5
#define APEX_INT32    6
#define APEX_INT64    7
```

**Note**: Floating-point types (float, double) are **not supported** in hot paths. Use fixed-point integer representations instead.

### ExecutionMode

```c
#define APEX_EXEC_MODE_BIT_SLICED  0  // Default: high throughput
#define APEX_EXEC_MODE_SCALAR      1  // Low latency: simple loop
```

| Mode | Throughput | Latency | Batch Size | Use Case |
|------|-----------|---------|-----------|----------|
| BIT_SLICED | 1.3B+ TPS | ~100ns per 64 rows | 64+ | High-volume queries |
| SCALAR | ~4M TPS | ~30µs per row | 1-2 | Single-row evaluation |

### Field Descriptor

```c
typedef struct {
    const char* name;          // Field name (e.g., "price")
    size_t offset;             // Byte offset in row
    size_t bit_width;          // Bit width (8, 16, 32, 64)
    int data_type;             // DataType enum value
} apex_field_descriptor_t;
```

---

## Error Codes

### Return Codes

```c
#define APEX_OK                0  // Success
#define APEX_ERR_INVALID_ARG   1  // Invalid argument
#define APEX_ERR_NOT_FOUND     2  // Schema/logic not found
#define APEX_ERR_ALLOC         3  // Memory allocation failed
#define APEX_ERR_COMPILE       4  // JIT compilation failed
#define APEX_ERR_EXEC          5  // Execution error
#define APEX_ERR_INVALID_MODE  6  // Invalid execution mode
```

### Error Handling

**C**:
```c
int status = apex_register_schema(...);
if (status != APEX_OK) {
    switch (status) {
        case APEX_ERR_INVALID_ARG:
            fprintf(stderr, "Invalid schema argument\n");
            break;
        case APEX_ERR_ALLOC:
            fprintf(stderr, "Memory allocation failed\n");
            break;
        default:
            fprintf(stderr, "Unknown error: %d\n", status);
    }
    apex_destroy(engine);
    return 1;
}
```

**C++**:
```cpp
try {
    engine.register_schema("trade", fields, stride);
    engine.set_logic("trade", ir, mode);
} catch (const std::runtime_error& e) {
    std::cerr << "Error: " << e.what() << "\n";
    // Exception automatically cleans up
}
```

**Python**:
```python
try:
    engine.register_schema("trade", fields, stride)
except Exception as e:
    print(f"Error: {e}")
    # Exception handling or raise
```

**Java**:
```java
try {
    engine.registerSchema("trade", fields, 16);
} catch (AarchGateException e) {
    System.err.println("Error: " + e.getMessage());
}
```

---

## Memory Management

### Ownership & Lifetime

| Component | Owner | Lifetime |
|-----------|-------|----------|
| Engine Handle | Caller | From `apex_create()` to `apex_destroy()` |
| Schema | Engine | Until engine destroyed |
| Logic IR | Caller | Can be freed after `apex_set_logic()` |
| Data Buffer | Caller | Must remain valid during `apex_execute()` |

### Allocation Strategy

```c
// ✓ CORRECT: Allocate once, reuse
uint8_t* data = malloc(128 * 1000000 * 16);
apex_execute(engine, data, 128000000);
apex_execute(engine, data, 128000000);  // Reuse
free(data);

// ✗ WRONG: Allocate per execute (GC pressure)
for (int i = 0; i < num_batches; i++) {
    uint8_t* data = malloc(batch_size * 16);
    apex_execute(engine, data, batch_size);
    free(data);  // Unnecessary allocation/deallocation
}
```

### Memory Limits

| Component | Limit | Notes |
|-----------|-------|-------|
| Max Fields per Schema | 256 | Hard limit in registry |
| Max Schemas | 1024 | Configurable at compile time |
| Max Data Buffer | 16GB | Address space limit on 64-bit systems |
| Max JIT Code | 10MB | Capped per-engine |

---

## Execution Modes

### BIT_SLICED (Default)

**When to Use**:
- Processing ≥64 rows
- Complex expressions (>5 conditions)
- High-throughput workloads

**Performance**:
- Throughput: 1.3B+ TPS
- Latency: ~100ns per 64 rows (deterministic)
- Memory: ~512 bytes working set per thread

**Implementation**:
1. Transpose 64 rows into bit-planes
2. Compile expression to ARM64 JIT kernel
3. Execute kernel on bit-planes
4. Accumulate match mask

### SCALAR (Low-Latency)

**When to Use**:
- Single-row evaluation
- <1000 rows/sec workload
- Minimal latency requirement

**Performance**:
- Latency: ~25.95ms for 128M rows
- Memory: Minimal (no transpose)
- Throughput: Limited (~4M TPS)

**Implementation**:
- Simple C++ loop
- Per-row comparison
- No SIMD, no JIT

---

## Concurrency & Core Management

AarchGate is designed to saturate high-performance ARM64 silicon. The engine includes advanced logic to distinguish between **Performance Cores (P-cores)** and **Efficiency Cores (E-cores)** on Apple Silicon and modern ARM processors.

### 1. Automatic Core Detection
By default, the `ApexEngine` constructor probes the hardware topology:
- **macOS (Apple M-Series)**: Uses `sysctl` to detect the exact number of P-cores.
- **Default Behavior**: Sets the internal thread pool size to the P-core count to ensure maximum throughput without "straggler" jitter from E-cores.

### 2. Manual Concurrency Control
You can override the core usage through the following mechanisms:

#### API (C++)
```cpp
engine.set_thread_count(8); // Explicitly use 8 threads
```

#### Environment Variable
The `APEX_THREADS` variable overrides all default settings and API calls during engine initialization.
```bash
export APEX_THREADS=6
./your_app
```

### 3. P-Core Isolation (QOS)
All parallel execution threads are spawned with the `QOS_CLASS_USER_INTERACTIVE` priority. On macOS, this signals the kernel to:
1.  Pin the thread to a Performance Core.
2.  Maintain the highest possible CPU frequency (boost).
3.  Minimize preemption by background system tasks.

---

## Schema Definition

### Best Practices

1. **Align fields naturally**: Place larger fields first
   ```c
   // ✓ GOOD: 8+4+4=16 (natural alignment, no padding)
   {"price", 0, 64, APEX_UINT64},
   {"volume", 8, 32, APEX_UINT32},
   {"symbol", 12, 32, APEX_UINT32},
   
   // ✗ WASTEFUL: 8+4+8 = 20 bytes, no 64-byte alignment
   {"price", 0, 64, APEX_UINT64},
   {"symbol", 8, 32, APEX_UINT32},
   {"qty", 12, 64, APEX_UINT64},
   ```

2. **Use fixed-width types**: No variable-length fields
   ```c
   // ✓ CORRECT
   {"symbol_code", 0, 32, APEX_UINT32},  // 4-byte enum
   
   // ✗ NOT SUPPORTED
   // {"symbol", 0, -1, APEX_STRING},  // Variable length
   ```

3. **Cache-line align hot data**: Place accessed fields in same cache line
   ```c
   struct Trade {  // stride=64 (one cache line)
       uint64_t price;    // 8 bytes
       uint32_t volume;   // 4 bytes
       uint32_t symbol;   // 4 bytes
       uint32_t reserved; // 4 bytes (padding)
       uint64_t ts;       // 8 bytes
       uint64_t id;       // 8 bytes
       uint32_t flags;    // 4 bytes
       uint32_t _pad;     // 4 bytes
   };  // Exactly 64 bytes
   ```

---

## Examples

### Complete C Example: 128M Row Benchmark

```c
#include <apex/apex_c_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main() {
    // Create engine
    apex_engine_h engine = apex_create();
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        return 1;
    }
    
    // Register schema
    apex_field_descriptor_t fields[] = {
        {"price", 0, 64, APEX_UINT64},
    };
    int status = apex_register_schema(engine, "trade", fields, 1, 8);
    if (status != 0) {
        fprintf(stderr, "Register failed: %d\n", status);
        apex_destroy(engine);
        return 1;
    }
    
    // Set logic: price > 25000
    void* logic = apex_create_simple_logic();
    status = apex_set_logic(engine, "trade", logic, APEX_EXEC_MODE_BIT_SLICED);
    if (status != 0) {
        fprintf(stderr, "Set logic failed: %d\n", status);
        apex_destroy(engine);
        return 1;
    }
    
    // Create data: 128M rows × 8 bytes
    size_t num_rows = 128 * 1000000;
    uint64_t* data = (uint64_t*)malloc(num_rows * sizeof(uint64_t));
    if (!data) {
        fprintf(stderr, "Memory allocation failed\n");
        apex_destroy(engine);
        return 1;
    }
    
    // Generate random prices
    for (size_t i = 0; i < num_rows; i++) {
        data[i] = (uint64_t)rand() % 65536;
    }
    
    // Time execution
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    uint64_t matches = apex_execute(engine, data, num_rows);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    if (matches == (uint64_t)-1) {
        fprintf(stderr, "Execution failed\n");
        free(data);
        apex_destroy(engine);
        return 1;
    }
    
    // Calculate elapsed time
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                        (end.tv_nsec - start.tv_nsec) / 1000000.0;
    double tps = num_rows / (elapsed_ms / 1000.0);
    
    printf("=== BENCHMARK RESULTS ===\n");
    printf("Rows:    %zu\n", num_rows);
    printf("Matches: %llu\n", (unsigned long long)matches);
    printf("Time:    %.2f ms\n", elapsed_ms);
    printf("TPS:     %.0f\n", tps);
    
    // Cleanup
    free(data);
    apex_destroy(engine);
    
    return 0;
}
```

### Complete Python Example: NumPy Integration

```python
import aarchgate_python as ag
import numpy as np
import time

def benchmark_numpy():
    # Create engine
    engine = ag.AarchGateEngine()
    
    # Register schema
    fields = [
        ag.Field("price", 0, 64, ag.UINT64),
    ]
    engine.register_schema("trade", fields, stride=8)
    
    # Set logic
    logic = ag.create_simple_logic()
    engine.set_logic("trade", logic, ag.BIT_SLICED)
    
    # Create NumPy array (128M rows)
    num_rows = 128_000_000
    data = np.zeros(num_rows, dtype=np.uint64)
    np.random.seed(42)
    data[:] = np.random.randint(0, 65536, num_rows)
    
    # Time execution
    start = time.monotonic()
    matches = engine.execute(data, num_rows)
    elapsed = time.monotonic() - start
    
    # Results
    print(f"=== BENCHMARK RESULTS ===")
    print(f"Rows:    {num_rows:,}")
    print(f"Matches: {matches:,}")
    print(f"Time:    {elapsed:.2f}s")
    print(f"TPS:     {num_rows / elapsed:.0f}")

if __name__ == "__main__":
    benchmark_numpy()
```

---

**SDK Reference Complete**

For issues or clarifications, consult:
- `README_AARCHLOGIC.md` — Feature overview
- `ARCHITECT_GUIDE.md` — Internal design
- `examples/` — Working code samples

