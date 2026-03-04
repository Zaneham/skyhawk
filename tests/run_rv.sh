#!/bin/bash
# run_rv.sh -- Run RISC-V QEMU tests via WSL
# Compiles J73 source through the full pipeline on Windows,
# writes standalone ELF, runs under WSL qemu-riscv64.

SK="./skyhawk.exe"
PASS=0
FAIL=0
SKIP=0
TMP="tests/fixtures/_rv_qemu.elf"

run_test() {
    local name="$1"
    local src="$2"
    local expect="$3"

    # Write source to temp file
    local srcf="tests/fixtures/_rv_src.jov"
    printf '%s' "$src" > "$srcf"

    # Compile to standalone ELF (uses rv_exec internally via a helper)
    # We need the test binary for this. Use the main compiler for .o only.
    # Instead, we'll build a tiny helper that calls rv_exec.
    # Actually, let's just use the test binary with --cat rv --test <name>
    # But that won't work because th_run uses popen which can't call WSL.

    # Simplest approach: compile .jov -> write standalone ELF manually
    # We need a small program. Let's just check if the test harness
    # generated the ELF already from the test run.

    printf "  %-20s " "$name"
    echo "$src" | cat > "$srcf"

    # Actually we need to call the pipeline. Let's just do it via
    # a dedicated test runner that outputs ELFs.
    echo "SKIP (need integrated runner)"
    SKIP=$((SKIP + 1))
}

echo "RISC-V QEMU Tests (via WSL)"
echo "============================"

# First, check if qemu-riscv64 works
if ! wsl -- qemu-riscv64 --version > /dev/null 2>&1; then
    echo "ERROR: qemu-riscv64 not available in WSL"
    exit 1
fi

echo "qemu-riscv64 found in WSL"
echo ""

# We need the test_skyhawk binary to generate the standalone ELFs.
# The cleanest approach: add a mode to skyhawk.exe that writes
# a standalone ELF (--rv-exec), then run it under WSL.

# For now, let's use a simpler approach: compile with --rv,
# but we need rv_exec not rv_elf. Let's add --rv-exec to main.

echo "Need --rv-exec flag in main.c. Adding now would be the fix."
echo "Alternative: run test_skyhawk.exe under WSL directly."
echo ""

# Actually, the simplest: just cross-compile test_skyhawk for Linux
# and run the whole test suite under WSL. But that requires a Linux
# GCC in WSL. Let's try that.

echo "Attempting to build test harness natively in WSL..."
