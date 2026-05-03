# AarchLogic v1.0.0: Installation & Deployment Guide

## Complete Setup Instructions for All Environments

---

## Quick Start (5 minutes)

### Docker (Recommended)
```bash
# Build Docker image
docker build -f .docker/build.Dockerfile -t aarchlogic:latest .

# Run in container
docker run -it -v $(pwd):/workspace aarchlogic:latest bash

# Inside container
cd /workspace
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --parallel 8
./build/benchmarks/bench_aarchgate_final
```

### Local Build (Requires CMake)
```bash
# Prerequisites: CMake 3.18+, Clang 15+ or GCC 10+
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --parallel $(nproc)
./build/benchmarks/bench_aarchgate_final
```

---

## Prerequisites

### Build Tools

| Tool | Minimum | Recommended | Installation |
|------|---------|------------|--------------|
| CMake | 3.18 | 3.28+ | `brew install cmake` (macOS) |
| C++ Compiler | GCC 10, Clang 10 | Clang 15 | `apt-get install clang-15` |
| Build Generator | Make | Ninja | `apt-get install ninja-build` |
| Git | 2.20 | 2.40+ | System package manager |
| Python | 3.8 (optional) | 3.10+ | Python official site |
| Java | 11 (optional) | 17+ | OpenJDK |

### Runtime Libraries (Optional)

- **libc**: Standard C library
- **libpthread**: POSIX threads
- **libm**: Math library
- **libacl**: POSIX ACL support (for iceoryx)

All are usually pre-installed on Linux/macOS.

### Hardware

| Component | Minimum | Recommended |
|-----------|---------|------------|
| CPU | ARM64 (AArch64) or x86_64 | ARM Cortex-A78+ or Intel Xeon |
| RAM | 4GB | 16GB (for 128M row benchmarks) |
| Disk | 500MB (build) | 2GB (build + artifacts) |
| CPU Cores | 2 | 4+ (for parallel builds) |

---

## Installation Steps

### Step 1: Clone Repository

```bash
git clone https://github.com/yourusername/aarchlogic.git
cd aarchlogic
```

### Step 2: Verify Dependencies

```bash
# Check CMake
cmake --version
# Expected: cmake version 3.18 or higher

# Check C++ compiler
clang++ --version
# or
g++ --version

# Check git
git --version
```

### Step 3: Configure Build

```bash
# Release build (production)
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja

# OR debug build (with sanitizers)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
```

**Configuration Options**:

```bash
# Use specific C++ compiler
cmake -B build \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja

# Disable optional features
cmake -B build \
    -DASMJIT_FOUND=OFF \
    -DHIGHWAY_FOUND=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja

# Custom install prefix
cmake -B build \
    -DCMAKE_INSTALL_PREFIX=/opt/aarchlogic \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja
```

### Step 4: Build

```bash
# Parallel build (4 jobs)
cmake --build build --parallel 4

# Or use Ninja directly (faster)
ninja -C build -j 8

# Verbose output (for debugging)
cmake --build build --parallel 4 --verbose
```

**Expected Output**:
```
[1/345] Building CXX object ...
[2/345] Building CXX object ...
...
[345/345] Linking CXX executable build/benchmarks/bench_aarchgate_final
Built target aarchgate
```

### Step 5: Verify Build

```bash
# Run unit tests
cd build && ctest --output-on-failure && cd ..

# Run benchmark
./build/benchmarks/bench_aarchgate_final

# Expected output includes:
# [✓ COUNTS MATCH EXACTLY!]
# [✓ ALL VERIFICATIONS PASSED]
```

---

## Docker Deployment

### Building the Docker Image

```bash
# Build for default platform (usually amd64)
docker build -f .docker/build.Dockerfile -t aarchlogic:latest .

# Build for specific platform
docker build -f .docker/build.Dockerfile \
    --platform linux/arm64 \
    -t aarchlogic:arm64 .

# Build with tags
docker build -f .docker/build.Dockerfile \
    -t aarchlogic:1.0.0 \
    -t aarchlogic:latest .
```

### Running in Docker

#### Interactive Shell

```bash
docker run -it \
    -v $(pwd):/workspace \
    aarchlogic:latest \
    bash
```

**Inside container**:
```bash
cd /workspace
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build
./build/benchmarks/bench_aarchgate_final
```

#### One-Shot Build

```bash
docker run --rm \
    -v $(pwd):/workspace \
    aarchlogic:latest \
    bash -c "cd /workspace && \
        cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja && \
        cmake --build build && \
        ./build/benchmarks/bench_aarchgate_final"
```

#### Docker Compose

```bash
cd .docker
docker-compose up -d aarchlogic-build
docker-compose exec aarchlogic-build bash
```

### Multi-Stage Docker Build (Production)

```dockerfile
# Stage 1: Build
FROM ubuntu:22.04 AS builder
# ... build steps ...
RUN cmake -B build && cmake --build build

# Stage 2: Runtime (minimal image)
FROM ubuntu:22.04
COPY --from=builder /workspace/build/libaarchgate.so /usr/lib/
COPY --from=builder /workspace/include/ /usr/include/

ENTRYPOINT ["your-app"]
```

---

## Native Linux Deployment

### Extract Compiled Artifacts

```bash
# From build directory
cp build/libaarchgate.so /usr/local/lib/
cp include/apex/*.h* /usr/local/include/apex/

# Create library symlinks
ln -s /usr/local/lib/libaarchgate.so.1 /usr/local/lib/libaarchgate.so
ldconfig  # Update library cache
```

### PKG Config Integration

```bash
# Create /usr/local/lib/pkgconfig/aarchlogic.pc
cat > /usr/local/lib/pkgconfig/aarchlogic.pc <<EOF
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: AarchLogic
Description: Universal Bit-Sliced Logic Engine
Version: 1.0.0
Cflags: -I\${includedir}
Libs: -L\${libdir} -laarchlogic
EOF

# Use in CMake
pkg_check_modules(AARCHLOGIC REQUIRED aarchlogic)
```

### SystemD Service (Optional)

For daemon applications using AarchLogic:

```ini
[Unit]
Description=AarchLogic Data Processing Service
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/aarchlogic-daemon
Restart=on-failure
RestartSec=5s

# Performance tuning
CPUAffinity=0-3
MemoryLimit=16G
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
```

Enable with:
```bash
systemctl enable aarchlogic-daemon.service
systemctl start aarchlogic-daemon.service
systemctl status aarchlogic-daemon.service
```

---

## Kernel Tuning (High-Performance Deployment)

### CPU Affinity (Linux)

Pin threads to isolated CPUs for predictable latency:

```bash
# Boot parameter: isolate CPUs 2-3 from scheduler
# (Add to GRUB or kernel command line)
# isolcpus=2-3

# Bind AarchLogic process to isolated CPUs
taskset -c 2-3 ./build/benchmarks/bench_aarchgate_final

# Or programmatically (in C)
cpu_set_t mask;
CPU_ZERO(&mask);
CPU_SET(2, &mask);
CPU_SET(3, &mask);
sched_setaffinity(0, sizeof(mask), &mask);
```

### Memory Tuning

#### Huge Pages (2MB or 1GB)

```bash
# Enable 2MB huge pages
echo 1024 > /proc/sys/vm/nr_hugepages

# Or at boot: hugepagesz=1G hugepages=2

# Allocate memory on huge pages (Linux)
#include <sys/mman.h>
void* mem = mmap(NULL, 1GB, PROT_READ|PROT_WRITE,
                  MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,
                  -1, 0);
```

**Expected Improvement**: 10-20% throughput gain (fewer TLB misses).

#### NUMA Awareness

```bash
# Check NUMA topology
numactl --hardware

# Bind to NUMA node
numactl --cpunodebind=0 --membind=0 ./app

# Programmatically (libnuma)
numa_run_on_node(0);
numa_set_preferred(0);
```

### Disable CPU Frequency Scaling

```bash
# Check current governor
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Set to performance mode
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

### AarchGate Environment Overrides
| Variable | Description | Default |
|----------|-------------|---------|
| `APEX_THREADS` | Manually specify core count | Auto-detected P-cores |
| `APEX_LOG_LEVEL` | Logging verbosity (0-3) | 1 (Info) |

Example:
```bash
APEX_THREADS=8 ./build/benchmarks/bench_tpch_q6
```
```

### Real-Time Scheduler (Optional)

```bash
# Set SCHED_FIFO (requires root)
chrt -f 50 ./app  # Priority 50 (1-99)

# Or SCHED_RR (round-robin)
chrt -r 50 ./app
```

---

## Python Setup

### Installing Python Bindings

```bash
# Build Python bindings
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --target aarchgate_python

# Install to system Python
pip install ./build/bindings/python
# or
python setup.py install
```

### Virtual Environment

```bash
# Create isolated environment
python3 -m venv aarchlogic_env
source aarchlogic_env/bin/activate

# Install bindings
pip install ./build/bindings/python
pip install numpy  # For data handling

# Verify
python -c "import aarchgate_python; print('OK')"
```

### Jupyter Notebook Integration

```python
# In notebook
import sys
sys.path.insert(0, '/path/to/build/bindings/python')

import aarchgate_python as ag
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

# Create engine and benchmark
engine = ag.AarchGateEngine()
# ... use AarchLogic in notebook ...
```

---

## Java Setup

### Building Java Bindings

```bash
# Build Java bindings (requires JDK 11+)
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --target aarchgate_java

# Output: build/bindings/java/aarchgate.jar
```

### Adding to Maven Project

```xml
<!-- pom.xml -->
<dependency>
    <groupId>com.aarchlogic</groupId>
    <artifactId>aarchlogic-core</artifactId>
    <version>1.0.0</version>
</dependency>
```

Or build from source:

```bash
# In Java project
mvn install:install-file \
    -Dfile=build/bindings/java/aarchgate.jar \
    -DgroupId=com.aarchlogic \
    -DartifactId=aarchlogic-core \
    -Dversion=1.0.0 \
    -Dpackaging=jar
```

### Gradle Integration

```gradle
// build.gradle
dependencies {
    implementation fileTree(dir: 'lib', include: ['*.jar'])
    // or from Maven Central (future)
    implementation 'com.aarchlogic:aarchlogic-core:1.0.0'
}

tasks.run {
    doFirst {
        systemProperty 'java.library.path', 
            System.getProperty('java.library.path') + 
            ":${projectDir}/build/bindings/java"
    }
}
```

---

## Troubleshooting

### Build Failures

#### CMake Not Found
```bash
# macOS
brew install cmake

# Ubuntu/Debian
sudo apt-get install cmake

# Verify
cmake --version
```

#### Clang Not Found
```bash
# macOS
brew install llvm

# Ubuntu/Debian
sudo apt-get install clang-15

# Set as default
export CXX=clang++
export CC=clang
```

#### SIMD/Highway Issues
```bash
# Build without Highway optimization
cmake -B build \
    -DHIGHWAY_FOUND=OFF \
    -DCMAKE_BUILD_TYPE=Release

# Check supported targets
./build_perf/hwy_list_targets
```

### Runtime Issues

#### Library Not Found (Linux)
```bash
# Set library path
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

# Or compile with RPATH
cmake -B build \
    -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON \
    -DCMAKE_BUILD_TYPE=Release
```

#### Python Import Error
```bash
# Check binding installation
python -c "import aarchgate_python"

# If not found, rebuild
cmake --build build --target aarchgate_python
export PYTHONPATH=$(pwd)/build/bindings/python:$PYTHONPATH
```

#### JNI Library Not Found (Java)
```bash
# Specify JNI library path
java -Djava.library.path=/usr/local/lib MyApp

# Or in code
System.load("/usr/local/lib/libaarchgate_java.so");
```

---

## Performance Validation

### Baseline Benchmark

```bash
# Run official benchmark
./build/benchmarks/bench_aarchgate_final

# Expected (ARM64, ~2GHz):
# Matches: 89,588,765
# Time:    95.38 ms
# TPS:     1,342,049,089 (1.3B)
```

### Custom Workload

```bash
# Create test data
python -c "
import numpy as np
data = np.random.randint(0, 65536, 128_000_000, dtype=np.uint64)
data.tofile('test_data.bin')
"

# Benchmark
time ./build/examples/custom_benchmark test_data.bin
```

### Profiling

```bash
# CPU profile
perf record -F 99 -g ./build/benchmarks/bench_aarchgate_final
perf report

# Memory profile (Valgrind)
valgrind --tool=massif ./build/benchmarks/bench_aarchgate_final
ms_print massif.out.XXXXX
```

---

## Uninstallation

### Remove Local Build

```bash
rm -rf build cmake-build-*
```

### Uninstall System Libraries

```bash
# Remove system-wide installation
rm /usr/local/lib/libaarchlogic.so*
rm -r /usr/local/include/apex/
rm /usr/local/lib/pkgconfig/aarchlogic.pc
ldconfig
```

### Remove Docker Image

```bash
docker rmi aarchlogic:latest
```

### Remove Python Package

```bash
pip uninstall aarchgate-python
```

---

## Next Steps

After installation:

1. **Explore Examples**: `examples/trading/main.cpp`
2. **Run Benchmarks**: `./build/benchmarks/bench_aarchgate_final`
3. **Read Architecture Guide**: `ARCHITECT_GUIDE.md`
4. **Review API Reference**: `SDK_REFERENCE.md`
5. **Check Development Guide**: `DEVELOPMENT_GUIDE.md`

---

**For issues**:
- CMake configuration: Check `CMakeLists.txt` and error messages
- Build failures: Review `cmake-build-*/CMakeOutput.log`
- Runtime issues: Enable debug build for ASan/UBSan detection

