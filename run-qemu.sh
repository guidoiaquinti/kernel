#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
usage() {
    cat <<'USAGE'
Usage: ./run-qemu.sh [options] [-- QEMU arguments...]
  --target NAME       qemu_raspi3b (default) or qemu_virt
  --debug | --release Select matching build configuration (default: Debug)
  --build-dir DIR     Build root, as in build.sh (default: build beside script)
  --smp N             virt: cores to emulate, 1 to 4 (default 1, or KORA_QEMU_SMP);
                      raspi3b always has 4
  --virt | --raspi3b   Deprecated aliases
Environment: KORA_QEMU_TARGET, KORA_QEMU_SMP, KORA_QEMU_RAM, KORA_QEMU_RAMFB,
 KORA_QEMU_KEYBOARD, KORA_QEMU_DISK, KORA_QEMU_DISK_READONLY,
 KORA_QEMU_FB and KORA_QEMU_DISPLAY retain their existing meanings.
Unrecognized arguments are passed to QEMU unchanged.
USAGE
}
fail() { echo "Error: $*" >&2; exit 1; }
require_value() { [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || fail "$1 requires a value"; }
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
target="${KORA_QEMU_TARGET:-qemu_raspi3b}"
build_root="${BUILD_DIR:-$script_dir/build}"
build_type="${BUILD_TYPE:-Debug}"
smp="${KORA_QEMU_SMP:-1}"
extra_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) require_value "$@"; target="$2"; shift 2 ;;
        --debug) build_type=Debug; shift ;;
        --release) build_type=Release; shift ;;
        --build-dir) require_value "$@"; build_root="$2"; shift 2 ;;
        --smp) require_value "$@"; smp="$2"; shift 2 ;;
        --virt) echo "Deprecated: --virt; use --target qemu_virt" >&2; target=qemu_virt; shift ;;
        --raspi3b) echo "Deprecated: --raspi3b; use --target qemu_raspi3b" >&2; target=qemu_raspi3b; shift ;;
        --help|-h) usage; exit 0 ;;
        --) shift; extra_args+=("$@"); break ;;
        *) extra_args+=("$1"); shift ;;
    esac
done
case "$target" in
    virt|raspi3b)
        echo "Deprecated: target $target; use qemu_$target" >&2
        target="qemu_$target" ;;
esac
case "$build_type" in Debug|debug) configuration=debug ;; Release|release) configuration=release ;; *) fail "BUILD_TYPE must be Debug or Release" ;; esac
case "$target" in qemu_raspi3b|qemu_virt) ;; *) fail "Unsupported QEMU target: $target" ;; esac
[[ "$smp" =~ ^[1-4]$ ]] || fail "--smp must be 1 to 4"
command -v python3 >/dev/null || fail "Python3 is required"
normalize_path() { python3 -c 'import os,sys; print(os.path.realpath(os.path.abspath(sys.argv[1])))' "$1"; }
build_root=$(normalize_path "$build_root")
kernel_img="$build_root/$configuration/$target/kernel.img"
[[ -s "$kernel_img" && -f "$kernel_img" ]] || fail "Missing $kernel_img; build with --target $target first"
if [[ "$target" == qemu_raspi3b ]]; then
    machine_args=(-M raspi3b)
else
    machine_args=(-M virt,gic-version=2,highmem=off -cpu cortex-a72 -smp "$smp" -nic none
        -global virtio-mmio.force-legacy=false -m "${KORA_QEMU_RAM:-256M}")
    if [[ "${KORA_QEMU_RAMFB:-1}" == 1 ]]; then machine_args+=(-device ramfb); fi
    if [[ "${KORA_QEMU_KEYBOARD:-1}" == 1 ]]; then machine_args+=(-device virtio-keyboard-device); fi
    if [[ -n "${KORA_QEMU_DISK:-}" ]]; then
        disk=$(normalize_path "$KORA_QEMU_DISK")
        [[ -f "$disk" ]] || fail "Missing disk: $disk"
        # QEMU's comma-separated drive syntax escapes literal commas by doubling.
        disk_option=${disk//,/,,}
        machine_args+=(-drive "if=none,id=root,format=raw,file=$disk_option,readonly=${KORA_QEMU_DISK_READONLY:-on}"
            -device virtio-blk-device,drive=root)
    fi
fi
display_args=(-display none)
if [[ "${KORA_QEMU_FB:-0}" == 1 ]]; then
    if [[ "$(uname -s)" == Darwin ]]; then default_display=cocoa; else default_display=gtk; fi
    display_args=(-display "${KORA_QEMU_DISPLAY:-$default_display}")
fi
echo "Starting KoraOS on $target; Ctrl-A X quits QEMU"
exec qemu-system-aarch64 "${machine_args[@]}" -kernel "$kernel_img" \
    -serial stdio "${display_args[@]}" ${extra_args[@]+"${extra_args[@]}"}
