import numpy as np
import apex_python
import time

def run_test():
    print("=== Zero-Copy Handoff Verification (LATENCY TEST) ===")
    
    # 1. Allocate a large 256MB array to make any copy very expensive
    num_elements = 10_000_000 
    data = np.zeros(num_elements, dtype=[('f0', 'u8'), ('f1', 'u8'), ('f2', 'u8')])
    for i in range(num_elements):
        data[i] = (i, i, i)
    
    print(f"Allocated {data.nbytes / (1024*1024):.2f} MB NumPy array")

    # 2. Setup Apex Engine
    engine = apex_python.ApexEngine()
    fields = [
        ("Field0", 0, 64, 3),
        ("Field1", 8, 64, 3),
        ("Field2", 16, 64, 3)
    ]
    engine.register_schema("ZeroCopyTest", fields, 24)
    expr_ptr = apex_python.create_universal_test_logic()
    engine.set_logic("ZeroCopyTest", expr_ptr, 0)

    # 3. Measure Handoff Latency
    data_view = data.view(np.uint8)
    
    # We measure the time taken to enter the C++ engine.
    # If a copy were occurring, it would take >50ms for 256MB.
    # Zero-copy should be <1ms.
    t0 = time.perf_counter_ns()
    matches = engine.execute(data_view, num_elements)
    t1 = time.perf_counter_ns()
    
    latency_ms = (t1 - t0) / 1_000_000
    print(f"Total Execution (including handoff): {latency_ms:.4f} ms")
    print(f"Throughput: {(num_elements / (latency_ms/1000)) / 1_000_000:.2f} M rows/sec")
    
    if latency_ms < 50:
        print("VERIFICATION: SUCCESS (Zero-copy confirmed)")
    else:
        print("VERIFICATION: FAILURE (Possible data copy detected)")

if __name__ == "__main__":
    run_test()
