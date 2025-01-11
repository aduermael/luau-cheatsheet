#!/bin/bash
set -e

# Create build directory
mkdir -p build
cd build

# Run CMake
cmake ..

# Build using all available cores
cmake --build . --parallel $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)

# Copy executable to parent directory
cp main ..
