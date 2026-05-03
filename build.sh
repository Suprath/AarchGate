#!/bin/bash

# AarchGate Build Script
# Handles both local and Docker-based builds

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
GENERATOR="${GENERATOR:-Ninja}"
USE_DOCKER="${USE_DOCKER:-0}"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

show_usage() {
    cat << EOF
Usage: ./build.sh [OPTIONS]

Options:
    --docker          Build inside Docker container
    --debug           Build with debug symbols and sanitizers
    --release         Build with optimizations (default)
    --clean           Remove build directory before building
    --test            Run tests after building
    --benchmark       Run benchmarks after building
    --help            Show this help message

Environment variables:
    BUILD_TYPE        Release (default) or Debug
    GENERATOR         Ninja (default) or Unix Makefiles
    USE_DOCKER        Set to 1 to use Docker

Examples:
    ./build.sh --release
    ./build.sh --docker --debug
    ./build.sh --clean --test
EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --docker)
                USE_DOCKER=1
                shift
                ;;
            --debug)
                BUILD_TYPE=Debug
                shift
                ;;
            --release)
                BUILD_TYPE=Release
                shift
                ;;
            --clean)
                print_info "Cleaning build directory..."
                rm -rf "${BUILD_DIR}"
                shift
                ;;
            --test)
                RUN_TESTS=1
                shift
                ;;
            --benchmark)
                RUN_BENCHMARK=1
                shift
                ;;
            --help)
                show_usage
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                show_usage
                exit 1
                ;;
        esac
    done
}

build_locally() {
    print_info "Building AarchGate locally..."
    print_info "Build type: ${BUILD_TYPE}"
    print_info "Generator: ${GENERATOR}"

    # Configure
    print_info "Configuring CMake..."
    cmake -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -G "${GENERATOR}" \
        "${SCRIPT_DIR}"

    # Build
    print_info "Building..."
    cmake --build "${BUILD_DIR}" --parallel 4

    print_info "Build complete: ${BUILD_DIR}/aarchgate"
}

build_docker() {
    print_info "Building AarchGate in Docker..."
    print_info "Build type: ${BUILD_TYPE}"

    if ! command -v docker &> /dev/null; then
        print_error "Docker is not installed or not in PATH"
        exit 1
    fi

    # Build Docker image if it doesn't exist
    if ! docker image inspect aarchgate:latest > /dev/null 2>&1; then
        print_info "Building Docker image..."
        docker build -f "${SCRIPT_DIR}/.docker/build.Dockerfile" \
            -t aarchgate:latest \
            "${SCRIPT_DIR}"
    fi

    # Run build inside Docker
    docker run --rm \
        -v "${SCRIPT_DIR}:/workspace" \
        aarchgate:latest \
        bash -c "cd /workspace && cmake -B build -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -G Ninja && ninja -C build"

    print_info "Docker build complete: ${SCRIPT_DIR}/build/aarchgate"
}

run_tests() {
    if [[ -f "${BUILD_DIR}/aarchgate" ]]; then
        print_info "Running tests..."
        cd "${BUILD_DIR}"
        ctest --verbose || print_warn "Some tests failed"
        cd "${SCRIPT_DIR}"
    else
        print_warn "No executable found; skipping tests"
    fi
}

run_benchmark() {
    if [[ -f "${BUILD_DIR}/aarchgate" ]]; then
        print_info "Running benchmarks..."
        "${BUILD_DIR}/aarchgate"
    else
        print_warn "No executable found; skipping benchmark"
    fi
}

# Main
parse_args "$@"

if [[ $USE_DOCKER -eq 1 ]]; then
    build_docker
else
    build_locally
fi

[[ $RUN_TESTS -eq 1 ]] && run_tests
[[ $RUN_BENCHMARK -eq 1 ]] && run_benchmark

print_info "✓ Build successful!"
