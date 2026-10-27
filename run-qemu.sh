#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
usage() {
    cat <<'USAGE'
Usage: ./run-qemu.sh [options] [-- QEMU arguments...]
  --target NAME       qemu_raspi3b (default), qemu_raspi4b or qemu_virt
  --debug | --release Select matching build configuration (default: Debug)
  --build-dir DIR     Build root, as in build.sh (default: build beside script)
  --smp N             virt: cores to emulate, 1 to 4 (default 1, or KORA_QEMU_SMP);
                      raspi3b and raspi4b always have 4
  --gic 2|3           virt: interrupt controller version (default 2)
  --accel hvf         virt on an Apple Silicon Mac: run on the host's own cores
                      (Hypervisor.framework, -cpu host, GICv3) instead of TCG
  --virt | --raspi3b   Deprecated aliases
Environment: KORA_QEMU_TARGET, KORA_QEMU_SMP, KORA_QEMU_RAM, KORA_QEMU_RAMFB,
 KORA_QEMU_KEYBOARD, KORA_QEMU_DISK, KORA_QEMU_DISK_READONLY,
 KORA_QEMU_FB and KORA_QEMU_DISPLAY retain their existing meanings.
 KORA_QEMU_SD=IMAGE puts an SD card in a raspi machine's slot, and on raspi3b
 KORA_QEMU_USB=IMAGE plugs in a USB drive (QEMU snapshots: an image is never
 changed unless KORA_QEMU_SD_WRITABLE=1 or KORA_QEMU_USB_WRITABLE=1; the
 kernel only writes to them when built --storage-writable).
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
gic=2
accel=tcg
extra_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) require_value "$@"; target="$2"; shift 2 ;;
        --debug) build_type=Debug; shift ;;
        --release) build_type=Release; shift ;;
        --build-dir) require_value "$@"; build_root="$2"; shift 2 ;;
        --smp) require_value "$@"; smp="$2"; shift 2 ;;
        --gic) require_value "$@"; gic="$2"; shift 2 ;;
        --accel) require_value "$@"; accel="$2"; shift 2 ;;
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
case "$target" in qemu_raspi3b|qemu_raspi4b|qemu_virt) ;; *) fail "Unsupported QEMU target: $target" ;; esac
[[ "$smp" =~ ^[1-4]$ ]] || fail "--smp must be 1 to 4"
case "$gic" in 2|3) ;; *) fail "--gic must be 2 or 3" ;; esac
case "$accel" in tcg) cpu_args=(-cpu cortex-a72) ;; hvf) cpu_args=(-accel hvf -cpu host); gic=3 ;;
    *) fail "--accel must be tcg or hvf" ;; esac
if [[ "$target" != qemu_virt && ( "$gic" != 2 || "$accel" != tcg ) ]]; then
    fail "--gic and --accel apply to --target qemu_virt"
fi
command -v python3 >/dev/null || fail "Python3 is required"
normalize_path() { python3 -c 'import os,sys; print(os.path.realpath(os.path.abspath(sys.argv[1])))' "$1"; }
build_root=$(normalize_path "$build_root")
kernel_img="$build_root/$configuration/$target/kernel.img"
[[ -s "$kernel_img" && -f "$kernel_img" ]] || fail "Missing $kernel_img; build with --target $target first"
if [[ "$target" == qemu_raspi* ]]; then
    machine_args=(-M "${target#qemu_}")
    if [[ -n "${KORA_QEMU_SD:-}" ]]; then
        card=$(normalize_path "$KORA_QEMU_SD")
        [[ -f "$card" ]] || fail "Missing SD card image: $card"
        card_option="format=raw,file=${card//,/,,}"
        [[ "${KORA_QEMU_SD_WRITABLE:-0}" == 1 ]] || card_option+=",snapshot=on"
        if [[ "$target" == qemu_raspi3b ]]; then
            machine_args+=(-drive "if=sd,$card_option")
        else
            # The Pi 4's slot is EMMC2; QEMU's if=sd would use the Arasan controller.
            machine_args+=(-drive "if=none,id=sdcard,$card_option"
                -device sd-card,drive=sdcard,bus=main-system-bus/generic-sdhci/sd-bus)
        fi
    fi
    if [[ -n "${KORA_QEMU_USB:-}" ]]; then
        # QEMU's raspi4b has no PCIe, so no xHCI for the Pi 4's USB ports.
        [[ "$target" == qemu_raspi3b ]] || fail "KORA_QEMU_USB requires --target qemu_raspi3b"
        stick=$(normalize_path "$KORA_QEMU_USB")
        [[ -f "$stick" ]] || fail "Missing USB drive image: $stick"
        stick_option="if=none,id=stick,format=raw,file=${stick//,/,,}"
        [[ "${KORA_QEMU_USB_WRITABLE:-0}" == 1 ]] || stick_option+=",snapshot=on"
        machine_args+=(-drive "$stick_option" -device usb-storage,drive=stick)
    fi
else
    machine_args=(-M "virt,gic-version=$gic,highmem=off" "${cpu_args[@]}" -smp "$smp" -nic none
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
