#!/usr/bin/env bash
set -e

# ==============================================================================
# Grocycle Pre-Checkin Compliance & Verification Script
# Runs code formatting, compilation, test suite, and packaging checks.
# ==============================================================================

GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}======================================================${NC}"
echo -e "${CYAN}        Grocycle Pre-Checkin Verification             ${NC}"
echo -e "${CYAN}======================================================${NC}"

# If running on host and Docker is available, run inside grocycle-dev container
if [ ! -f /.dockerenv ] && command -v docker &>/dev/null; then
    echo -e "Running verification inside container 'grocycle-dev'..."
    exec docker run --rm -v "$(pwd):/workspace" grocycle-dev ./scripts/verify.sh
fi

echo -e "\n${CYAN}[1/5] Auto-formatting code (clang-format)...${NC}"
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make format

echo -e "\n${CYAN}[2/5] Compiling Grocycle (C++20, -Wall -Wextra -Wpedantic)...${NC}"
make -j$(nproc)

echo -e "\n${CYAN}[3/5] Running automated unit test suite...${NC}"
ctest --output-on-failure

echo -e "\n${CYAN}[4/5] Running static analysis (clang-tidy)...${NC}"
make lint

echo -e "\n${CYAN}[5/5] Building distribution packages (.deb and .tar.gz)...${NC}"
make package

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN}  ✓ All checks passed! Your code is ready to check in. ${NC}"
echo -e "${GREEN}======================================================${NC}"
