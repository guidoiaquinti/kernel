#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Assemble a FAT32 boot volume from already-built hardware kernels. No mounts.
set -euo pipefail
usage() {
    cat <<'USAGE'
Usage: ./create-sd-image.sh [options]
  --target NAME       Repeatable: hw_raspi3b, hw_raspi4b or all (default: both)
  --debug | --release Kernel configuration (default: Debug)
  --build-dir DIR     Build root, as in build.sh
  --userfs-dir DIR    Shared userfs whose programs and files go on the card
                      (default: BUILD_ROOT/userfs/aarch64)
  --staging DIR       Consume an existing complete boot payload instead
  --output FILE       Image path (default: BUILD_ROOT/koraos-boot.img)
  --volume LABEL      FAT label (default: KORAOS; up to 11 ASCII characters)
  --size SIZE_MB      Volume size in MiB (64..4096; default: automatic)
  --kernel FILE       Deprecated single-kernel mode, installed as kernel8.img
  --help              Show this help
Requires Python3 and mtools (mformat/mcopy); no root, loop device or mount.
Canonical kernels retain kernel8-rpi3.img / kernel8-rpi4.img firmware names.
--staging is a read-only source directory, not a staging destination.
USAGE
}
fail() { echo "Error: $*" >&2; exit 1; }
require_value() { [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || fail "$1 requires a value"; }
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_root="${BUILD_DIR:-$script_dir/build}"
build_type="${BUILD_TYPE:-Debug}"
staging_source=""
userfs_dir=""
image_path=""
kernel_source=""
volume_label=KORAOS
size_override=""
targets=()
add_target() {
    local name="$1" existing
    if [[ "$name" == all ]]; then add_target hw_raspi3b; add_target hw_raspi4b; return; fi
    case "$name" in hw_raspi3b|hw_raspi4b) ;; *) fail "SD image requires a hardware target: $name" ;; esac
    for existing in "${targets[@]:-}"; do [[ "$existing" != "$name" ]] || return 0; done
    targets+=("$name")
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) require_value "$@"; add_target "$2"; shift 2 ;;
        --debug) build_type=Debug; shift ;;
        --release) build_type=Release; shift ;;
        --build-dir) require_value "$@"; build_root="$2"; shift 2 ;;
        -s|--staging) require_value "$@"; staging_source="$2"; shift 2 ;;
        --userfs-dir) require_value "$@"; userfs_dir="$2"; shift 2 ;;
        -o|--output) require_value "$@"; image_path="$2"; shift 2 ;;
        --kernel) require_value "$@"; kernel_source="$2"; shift 2 ;;
        --volume) require_value "$@"; volume_label="$2"; shift 2 ;;
        --size) require_value "$@"; size_override="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) fail "Unknown option: $1" ;;
    esac
done
[[ -z "$kernel_source" || ${#targets[@]} -eq 0 ]] || fail "--kernel cannot be combined with --target"
[[ -z "$kernel_source" || -z "$staging_source" ]] || fail "--kernel cannot be combined with --staging"
[[ ${#targets[@]} -gt 0 ]] || add_target all
case "$build_type" in Debug|debug) configuration=debug ;; Release|release) configuration=release ;; *) fail "BUILD_TYPE must be Debug or Release" ;; esac
for tool in python3 mformat mcopy; do command -v "$tool" >/dev/null || fail "Missing $tool (install Python3/mtools)"; done
normalize_path() { python3 -c 'import os,sys; print(os.path.realpath(os.path.abspath(sys.argv[1])))' "$1"; }
build_root=$(normalize_path "$build_root")
image_path=$(normalize_path "${image_path:-$build_root/koraos-boot.img}")
[[ ! -e "$image_path" || -f "$image_path" ]] || fail "Output must be a regular image file: $image_path"
python3 - "$volume_label" "$size_override" <<'PY'
import re, sys
label, size = sys.argv[1:]
if not re.fullmatch(r"[A-Za-z0-9_ -]{1,11}", label):
    sys.exit("Invalid FAT volume label (use 1..11 ASCII letters, digits, spaces, '_' or '-')")
if size and (not re.fullmatch(r"[0-9]+", size) or not 64 <= int(size) <= 4096):
    sys.exit("Image size must be an integer from 64 to 4096 MiB")
PY
kernel_files=()
kernel_names=()
if [[ -n "$staging_source" ]]; then
    staging_source=$(normalize_path "$staging_source")
    [[ -d "$staging_source" ]] || fail "Missing boot payload: $staging_source"
    [[ "$image_path" != "$staging_source/"* ]] || fail "Output image cannot be inside its source payload"
    firmware_source="$staging_source"
    for target in "${targets[@]}"; do
        case "$target" in hw_raspi3b) name=kernel8-rpi3.img ;; hw_raspi4b) name=kernel8-rpi4.img ;; esac
        [[ -s "$staging_source/$name" && -f "$staging_source/$name" ]] || fail "Missing staged kernel: $staging_source/$name"
    done
    [[ -s "$staging_source/config.txt" ]] || fail "Missing staged config.txt"
else
    firmware_source="$script_dir/firmware"
    if [[ -n "$kernel_source" ]]; then
        echo "Deprecated: --kernel; use canonical --target hardware names" >&2
        kernel_files+=("$(normalize_path "$kernel_source")")
        kernel_names+=(kernel8.img)
    else
        for target in "${targets[@]}"; do
            kernel_files+=("$build_root/$configuration/$target/kernel.img")
            case "$target" in hw_raspi3b) kernel_names+=(kernel8-rpi3.img) ;; hw_raspi4b) kernel_names+=(kernel8-rpi4.img) ;; esac
        done
    fi
    for file in "${kernel_files[@]}"; do [[ -s "$file" && -f "$file" ]] || fail "Missing hardware kernel: $file; build first"; done
    userfs_image="$(normalize_path "${userfs_dir:-$build_root/userfs/aarch64}")/koraos.img"
    [[ -s "$userfs_image" && -f "$userfs_image" ]] || fail "Missing shared userfs: $userfs_image; build first"
fi
# Both board generations' firmware is retained, just as install_hw does.
for file in bootcode.bin start.elf start4.elf fixup.dat fixup4.dat LICENCE.broadcom \
    bcm2710-rpi-3-b.dtb bcm2711-rpi-4-b.dtb; do
    [[ -s "$firmware_source/$file" && -f "$firmware_source/$file" ]] || fail "Missing boot firmware: $firmware_source/$file"
done
stage_temp=$(mktemp -d "${TMPDIR:-/tmp}/koraos-boot.XXXXXX")
image_temp=""
cleanup() {
    rm -rf -- "$stage_temp"
    if [[ -n "$image_temp" ]]; then rm -f -- "$image_temp"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cp -a "$firmware_source/." "$stage_temp/"
if [[ -z "$staging_source" ]]; then
    for ((i = 0; i < ${#kernel_files[@]}; i++)); do
        cp "${kernel_files[i]}" "$stage_temp/${kernel_names[i]}"
    done
    # The user programs and files: a kernel without a ramdisk boots from the
    # card's volume that has /bin/init.
    LC_ALL=C.UTF-8 mcopy -s -n -Q -i "$userfs_image" "::/*" "$stage_temp/"
    if [[ -z "$kernel_source" ]]; then
        cp "$script_dir/config.txt" "$stage_temp/config.txt"
    else
        cat > "$stage_temp/config.txt" <<'CONFIG'
arm_64bit=1
enable_uart=1
uart_2ndstage=1
kernel=kernel8.img
[pi3]
core_freq=250
[pi4]
enable_gic=1
[all]
CONFIG
    fi
fi
# Sum logical file sizes, not host filesystem allocation, then reserve enough
# room for FAT metadata and growth. FAT32 volumes use at least 64 MiB here.
total_size_mb=$(python3 - "$stage_temp" "$size_override" <<'PY'
import os, sys
source, override = sys.argv[1:]
size = sum(os.path.getsize(os.path.join(root, name)) for root, _, names in os.walk(source) for name in names)
minimum = max(64, (size + 8 * 1024 * 1024 + 1024 * 1024 - 1) // (1024 * 1024))
requested = int(override) if override else minimum
if requested < minimum:
    sys.exit(f"Image size too small: need at least {minimum} MiB")
if requested > 4096:
    sys.exit("Boot payload exceeds maximum 4096 MiB image size")
print(requested)
PY
)
image_dir=$(dirname "$image_path")
mkdir -p "$image_dir"
image_temp=$(mktemp "$image_dir/.koraos-boot-image.XXXXXX")
python3 - "$image_temp" "$total_size_mb" <<'PY'
import sys
with open(sys.argv[1], "wb") as image:
    image.truncate(int(sys.argv[2]) * 1024 * 1024)
PY
mformat -F -v "$volume_label" -i "$image_temp" ::
shopt -s nullglob dotglob
entries=("$stage_temp"/*)
mcopy -s -Q -i "$image_temp" "${entries[@]}" ::/
# Publish after both formatting and population succeed; preserve an old image
# if any preparation step fails.
mv -f -- "$image_temp" "$image_path"
echo "Hardware FAT32 boot image ready: $image_path ($total_size_mb MiB)"
