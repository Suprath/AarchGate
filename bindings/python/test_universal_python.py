import sys
import time
import numpy as np

try:
    import apex_python
except ImportError:
    print("Could not import apex_python. Ensure the module is built and in PYTHONPATH.")
    sys.exit(1)

print("=== Universal Python SDK Verification Test ===")

engine = apex_python.ApexEngine()

fields = [
    ("Field0", 0, 64, 3), # 3 = UINT64
    ("Field1", 8, 64, 3),
    ("Field2", 16, 64, 3)
]

engine.register_schema("GenericSchema", fields, 64)

# Create the test IR logic: (Field0 + Field1) > Field2
logic_ptr = apex_python.create_universal_test_logic()
engine.set_logic("GenericSchema", logic_ptr, 1) # 1 = SCALAR

num_rows = 1_000_000

# Create structured array matching the C++ memory layout (64-byte chunks)
# We use a custom dtype to ensure the exact same byte offsets
dt = np.dtype([
    ('f0', np.uint64),
    ('f1', np.uint64),
    ('f2', np.uint64),
    ('pad', 'V40') # 40 bytes of padding to make it 64 bytes total
], align=True)

data = np.zeros(num_rows, dtype=dt)
data['f0'] = np.arange(num_rows, dtype=np.uint64)
data['f1'] = 1000

even_mask = (data['f0'] % 2 == 0)
data['f2'] = np.where(even_mask, data['f0'] + 500, data['f0'] + 2000)

# Execute Engine
start = time.time()
# Pass the raw underlying bytes to the engine
matches = engine.execute(data.view(np.uint8), num_rows)
end = time.time()

print(f"Matches: {matches} (Expected: 500000)")
print(f"Time: {(end - start) * 1000:.2f} ms")

if matches == 500000:
    print("[PASS] Universal Python SDK Test")
else:
    print("[FAIL] Incorrect matches")
    sys.exit(1)
