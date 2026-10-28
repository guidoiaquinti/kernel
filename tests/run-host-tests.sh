#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run the host-side unit tests: pure-logic kernel sources compiled
# with the host compiler under AddressSanitizer + UndefinedBehaviorSanitizer.
# No cross toolchain, QEMU or hardware needed. Exits non-zero on any failure.
#
# Usage: tests/run-host-tests.sh        (CC=... to pick the host compiler)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/host-tests"
CC="${CC:-clang}"

# -Wno-incompatible-library-redeclaration: the kernel's lib/string.h declares
# strlen() returning int, which differs from the host libc's size_t.
CFLAGS=(
    -std=c11 -g -O1
    -Wall -Wextra -Werror -Wno-incompatible-library-redeclaration
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
    -I "$ROOT/include" -I "$ROOT/tests/host"
    -DPRINTF_LONG_SUPPORT  # as in the kernel build (%l formats)
)

mkdir -p "$OUT"
failed=0

# run_test <name> <kernel sources...>: tests/host/<name>_test.c plus the kernel
# sources it exercises. Extra compiler flags for one test go in TEST_FLAGS.
run_test() {
    local name="$1"
    shift
    local sources=()
    for src in "$@"; do
        sources+=("$ROOT/$src")
    done
    echo "== $name"
    if ! "$CC" "${CFLAGS[@]}" ${TEST_FLAGS[@]+"${TEST_FLAGS[@]}"} -o "$OUT/${name}_test" \
            "$ROOT/tests/host/${name}_test.c" "${sources[@]}"; then
        echo "   BUILD FAILED"
        failed=1
        return
    fi
    if ! "$OUT/${name}_test"; then
        failed=1
    fi
}

python3 "$ROOT/tests/check-syscall-abi.py" || failed=1
python3 "$ROOT/tests/run-write-image-tests.py" || failed=1
python3 "$ROOT/tests/run-ramdisk-image-tests.py" || failed=1
run_test cp
run_test shell_input
run_test blkdev src/fs/blkdev.c
run_test term src/video/term.c
run_test printf src/lib/printf.c
run_test virt src/platform/virt.c
run_test placement src/proc/placement.c
run_test sd src/drivers/sd.c
run_test readahead src/drivers/readahead.c
run_test ramdisk_image src/fs/ramdisk_image.c
TEST_FLAGS=(-DKORAOS_HOST_TEST)
run_test kmalloc src/mm/kmalloc.c src/mm/kmalloc_stress.c src/arch/spinlock.c
TEST_FLAGS=(-DKORAOS_HOST_TEST -pthread)
run_test spinlock src/arch/spinlock.c
# The same test on the compiler-atomic fallback (what non-arm64 hosts build).
TEST_FLAGS=(-DKORAOS_HOST_TEST -DSPINLOCK_PORTABLE -pthread -I "$ROOT/tests/host")
"$CC" "${CFLAGS[@]}" "${TEST_FLAGS[@]}" -o "$OUT/spinlock_portable_test" \
    "$ROOT/tests/host/spinlock_test.c" "$ROOT/src/arch/spinlock.c" && "$OUT/spinlock_portable_test" || failed=1
TEST_FLAGS=()
run_test user_mem src/proc/user_mem.c
TEST_FLAGS=(-DLIBK_HOST_TEST -I "$ROOT/user/libk")
run_test libk_malloc user/libk/malloc.c user/libk/mutex.c
TEST_FLAGS=(-DLIBK_HOST_TEST -pthread -I "$ROOT/user/libk")
run_test libk_threads user/libk/malloc.c user/libk/mutex.c user/libk/job.c
TEST_FLAGS=()
"$ROOT/tests/run-filesystem-tests.sh" || failed=1
"$ROOT/tests/run-fat-write-tests.sh" || failed=1
"$ROOT/tests/run-fat-namespace-tests.sh" || failed=1
"$ROOT/tests/run-ramfb-tests.sh" || failed=1
"$ROOT/tests/run-virtio-tests.sh" || failed=1
"$ROOT/tests/run-virtio-input-tests.sh" || failed=1

if [ "$failed" -ne 0 ]; then
    echo "host tests: FAILED"
    exit 1
fi
echo "host tests: all passed"
