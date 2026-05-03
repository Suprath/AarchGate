# AarchGate Docker Toolchain

This directory contains Docker configuration for building AarchGate in a containerized environment with all dependencies pre-installed.

## Files

- **build.Dockerfile**: Multi-stage Docker image with Clang-16, CMake, Ninja, and ULL library dependencies
- **docker-compose.yml**: Development environment orchestration

## Quick Start

### Build Docker Image

```bash
docker build -f .docker/build.Dockerfile -t aarchgate:latest .
```

### Using Docker Compose (Recommended)

Start an interactive build container:

```bash
docker-compose -f .docker/docker-compose.yml up -d
docker-compose -f .docker/docker-compose.yml exec aarchgate-build bash
```

Inside the container:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja -C build
./build/aarchgate
```

### Using Docker Directly

Run a one-shot build:

```bash
docker run --rm -v $(pwd):/workspace aarchgate:latest bash -c \
  "cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja && ninja -C build"
```

Run an interactive container:

```bash
docker run -it --rm -v $(pwd):/workspace aarchgate:latest
```

## CLion Integration

To use this Docker image as a CLion toolchain:

1. **Settings** → **Tools** → **Docker**
   - Verify Docker is configured

2. **Settings** → **Tools** → **CMake** → **Toolchain**
   - Click **+** to add a new toolchain
   - Name: `aarchgate-docker`
   - Toolchain: Docker
   - Docker image: `aarchgate:latest`
   - CMake: `/usr/bin/cmake`
   - Make: `/usr/bin/ninja`
   - C Compiler: `/usr/bin/clang`
   - C++ Compiler: `/usr/bin/clang++`

3. Select the **aarchgate-docker** toolchain for your CMake profile

4. In **CMake** → **Profiles**, set:
   - Toolchain: `aarchgate-docker`
   - Build type: `Release` or `Debug`
   - Generator: `Ninja`

## Environment Variables

The Docker image sets:

- `CC=clang`
- `CXX=clang++`
- `LD=lld`
- `CXXFLAGS=-O3 -march=native -ffast-math -fno-finite-math-only`
- `CMAKE_BUILD_PARALLEL_LEVEL=4`

## Installed Tools

- **Compiler**: Clang 16 (with LLDB debugger)
- **Build System**: CMake, Ninja
- **Profiling**: perf, valgrind
- **Development**: Git, Python 3, development libraries
- **Libraries**: All dependencies (AsmJit, Highway, iceoryx, FlatBuffers) will be built from source during CMake configuration

## Building Dependencies

The Dockerfile does **not** pre-build AsmJit, Highway, iceoryx, or FlatBuffers; they are built during CMake configuration from the git submodules in `/external`. This ensures:

- Latest code from submodules
- Build customization via CMake cache variables
- Faster Docker image updates (dependencies don't need re-building)

To update dependencies:

```bash
git submodule update --remote
docker build -f .docker/build.Dockerfile -t aarchgate:latest .
```

## Debugging in Docker

Run with debugger:

```bash
docker run -it --rm -v $(pwd):/workspace aarchgate:latest \
  bash -c "cmake -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja && ninja -C build && lldb ./build/aarchgate"
```

Or use CLion's Docker integration with the aarchgate-docker toolchain.
