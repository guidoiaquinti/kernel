#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: ./build.sh [options] [clean]
  --target NAME       Repeatable: hw_raspi3b, hw_raspi4b, qemu_raspi3b,
                      qemu_raspi4b, qemu_virt or all (default: all five;
                      first-seen order)
  --debug | --release Kernel configuration (default: Debug)
  --build-dir DIR     Build root (default: build beside this script)
  --userfs-only       Build only the shared AArch64 user programs/FAT image
  --userfs-dir DIR    Consume DIR/koraos.img without rebuilding userfs
  --install-to DIR    Install selected hardware targets after all builds pass
  --storage-writable  Let the kernel write to SD cards and USB drives (default:
                      read-only); --sd-writable is an alias
  --clean             Clean selected configuration/targets and owned userfs first
  clean               Clean those outputs and exit
  --help              Show this help
Legacy --qemu, --virt, --hw, --variant and --all flags are deprecated aliases.
Userfs generation/consumption is serialized. Python3 is required for path/locking.
USAGE
}
fail() { echo "Error: $*" >&2; exit 1; }
warn() { echo "Deprecated: $*" >&2; }
require_value() { [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || fail "$1 requires a value"; }
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
original_args=("$@")
build_root="${BUILD_DIR:-$script_dir/build}"
build_type="${BUILD_TYPE:-Debug}"
storage_writable=OFF
userfs_override=""
install_dir=""
userfs_only=0
clean_first=0
clean_only=0
targets=()

add_target() {
    local name="$1" existing
    if [[ "$name" == all ]]; then
        add_target hw_raspi3b; add_target hw_raspi4b
        add_target qemu_raspi3b; add_target qemu_raspi4b; add_target qemu_virt
        return
    fi
    case "$name" in hw_raspi3b|hw_raspi4b|qemu_raspi3b|qemu_raspi4b|qemu_virt) ;; *) fail "Unknown target: $name" ;; esac
    for existing in "${targets[@]:-}"; do [[ "$existing" != "$name" ]] || return 0; done
    targets+=("$name")
}
legacy_target() {
    local alias="$1"
    warn "$alias; use --target with a canonical name"
    case "$alias" in
        qemu) add_target qemu_raspi3b ;;
        virt) add_target qemu_virt ;;
        hw|hardware)
            case "${RPI_VERSION:-4}" in
                3) add_target hw_raspi3b ;; 4) add_target hw_raspi4b ;;
                *) fail "Legacy --hw requires RPI_VERSION=3 or 4" ;;
            esac ;;
        all|both) add_target all ;;
        *) fail "Unknown legacy variant: $alias" ;;
    esac
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) require_value "$@"; add_target "$2"; shift 2 ;;
        --variant|-v) require_value "$@"; legacy_target "$2"; shift 2 ;;
        --qemu) legacy_target qemu; shift ;;
        --virt) legacy_target virt; shift ;;
        --hw|--hardware) legacy_target hw; shift ;;
        --all|--both) legacy_target all; shift ;;
        --debug) build_type=Debug; shift ;;
        --release) build_type=Release; shift ;;
        --build-dir) require_value "$@"; build_root="$2"; shift 2 ;;
        --userfs-dir) require_value "$@"; userfs_override="$2"; shift 2 ;;
        --install-to) require_value "$@"; install_dir="$2"; shift 2 ;;
        --storage-writable|--sd-writable) storage_writable=ON; shift ;;
        --userfs-only) userfs_only=1; shift ;;
        --clean) clean_first=1; shift ;;
        clean) clean_only=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) fail "Unknown option: $1" ;;
    esac
done
[[ ${#targets[@]} -gt 0 ]] || add_target all
case "$build_type" in Debug|debug) build_type=Debug; configuration=debug ;; Release|release) build_type=Release; configuration=release ;; *) fail "BUILD_TYPE must be Debug or Release" ;; esac
[[ $userfs_only -eq 0 || -z "$userfs_override" ]] || fail "--userfs-only and --userfs-dir cannot be combined"
[[ $userfs_only -eq 0 || -z "$install_dir" ]] || fail "--userfs-only cannot install kernels"
[[ $clean_only -eq 0 || -z "$install_dir" ]] || fail "clean cannot install kernels"
hardware_selected=0
for target in "${targets[@]}"; do
    case "$target" in hw_*) hardware_selected=1 ;; esac
done
[[ -z "$install_dir" || $hardware_selected -eq 1 ]] || fail "--install-to requires a selected hardware target"
command -v python3 >/dev/null || fail "Python3 is required"
normalize_path() { python3 -c 'import os,sys; print(os.path.realpath(os.path.abspath(sys.argv[1])))' "$1"; }
build_root=$(normalize_path "$build_root")
# Reject broad roots and ancestors of the checkout before any generated deletion.
python3 - "$build_root" "$script_dir" <<'PY'
import os, sys
root, checkout = sys.argv[1:]
broad = {os.path.realpath(path) for path in ("/", "/tmp", "/var/tmp", os.path.expanduser("~"))}
if root in broad or root == checkout or checkout.startswith(root + os.sep):
    sys.exit("Unsafe build root: " + root)
PY
if [[ -n "$userfs_override" ]]; then
    userfs_dir=$(normalize_path "$userfs_override")
    [[ -s "$userfs_dir/koraos.img" && -f "$userfs_dir/koraos.img" ]] || fail "Prepared userfs image missing or empty: $userfs_dir/koraos.img"
else
    userfs_dir="$build_root/userfs/aarch64"
fi
if [[ -n "$install_dir" ]]; then install_dir=$(normalize_path "$install_dir"); fi
# Validate every prospective clean path first, protecting prepared userfs even
# when a caller places it inside a selected target directory.
if [[ $clean_only -eq 1 || $clean_first -eq 1 ]]; then
    for target in "${targets[@]}"; do
        path="$build_root/$configuration/$target"
        if [[ -n "$userfs_override" && ( "$userfs_dir" == "$path" || "$userfs_dir" == "$path/"* ) ]]; then
            fail "Clean target contains prepared userfs: $path"
        fi
        [[ ! -L "$path" && "$(normalize_path "$path")" == "$path" ]] || fail "Refusing to clean a symlink path: $path"
    done
    if [[ -z "$userfs_override" ]]; then
        [[ ! -L "$userfs_dir" && "$(normalize_path "$userfs_dir")" == "$userfs_dir" ]] || fail "Refusing to clean a symlink userfs path: $userfs_dir"
    fi
fi
mkdir -p "$build_root"
# Keep the lock outside the producer directory; --clean must not remove it.
# All operations in this build root share the lock, including prepared-image
# consumers and cleans. External prepared images are the caller's responsibility.
lock_path="$build_root/.userfs-aarch64.lock"
lock_inherited=0
if [[ "${KORAOS_USERFS_LOCK_PATH:-}" == "$lock_path" && -n "${KORAOS_USERFS_LOCK_FD:-}" ]]; then
    if python3 - "$KORAOS_USERFS_LOCK_FD" "$lock_path" <<'PYLOCK'
import os, sys
try:
    inherited = os.fstat(int(sys.argv[1]))
    expected = os.stat(sys.argv[2])
    valid = (inherited.st_dev, inherited.st_ino) == (expected.st_dev, expected.st_ino)
except (ValueError, OSError):
    valid = False
sys.exit(0 if valid else 1)
PYLOCK
    then lock_inherited=1; fi
fi
if [[ $lock_inherited -eq 0 ]]; then
    exec python3 "$script_dir/scripts/with-userfs-lock.py" "$lock_path" "$script_dir/build.sh" ${original_args[@]+"${original_args[@]}"}
fi
if [[ $clean_only -eq 1 || $clean_first -eq 1 ]]; then
    if [[ $userfs_only -eq 0 ]]; then
        for target in "${targets[@]}"; do rm -rf -- "$build_root/$configuration/$target"; done
        rm -f -- "$build_root/compile_commands.json"
    fi
    if [[ -z "$userfs_override" ]]; then rm -rf -- "$userfs_dir"; fi
    echo "Cleaned selected outputs under $build_root"
    [[ $clean_only -eq 0 ]] || exit 0
fi
jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
if [[ -z "$userfs_override" ]]; then
    cmake -S "$script_dir/cmake/userfs" -B "$userfs_dir" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE="$build_type"
    cmake --build "$userfs_dir" --parallel "$jobs"
    [[ -s "$userfs_dir/koraos.img" ]] || fail "Producer did not create $userfs_dir/koraos.img"
fi
if [[ $userfs_only -eq 1 ]]; then echo "Userfs ready: $userfs_dir/koraos.img"; exit 0; fi
copied_commands=0
for target in "${targets[@]}"; do
    target_dir="$build_root/$configuration/$target"
    configure_args=(-S "$script_dir" -B "$target_dir" -G "Unix Makefiles"
        "-DCMAKE_BUILD_TYPE=$build_type" "-DKORAOS_TARGET=$target" "-DKORAOS_USERFS_DIR=$userfs_dir"
        "-DKORAOS_STORAGE_WRITABLE=$storage_writable")
    if [[ -n "$install_dir" && "$target" == hw_* ]]; then configure_args+=("-DBOOTMNT=$install_dir"); fi
    cmake "${configure_args[@]}"
    cmake --build "$target_dir" --parallel "$jobs"
    if [[ $copied_commands -eq 0 && -f "$target_dir/compile_commands.json" ]]; then
        cp "$target_dir/compile_commands.json" "$build_root/compile_commands.json"
        copied_commands=1
    fi
    echo "Built $target: $target_dir/kernel.img"
done
# Nothing is installed until every selected kernel has built successfully.
if [[ -n "$install_dir" ]]; then
    for target in "${targets[@]}"; do
        if [[ "$target" == hw_* ]]; then
            cmake --build "$build_root/$configuration/$target" --target install_hw
        fi
    done
    echo "Hardware boot files installed in $install_dir"
fi
