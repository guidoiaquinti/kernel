cmake_minimum_required(VERSION 3.20)

# Set the toolchain file before project()
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/aarch64-toolchain.cmake" CACHE PATH "Toolchain file")

project(KoraOS C CXX ASM)

# Enable compile_commands.json generation for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Modern configurations select one canonical target. Direct legacy CMake
# configurations retain the old variant switches and artifact names.
set(KORAOS_TARGET "" CACHE STRING "Canonical kernel target")
set_property(CACHE KORAOS_TARGET PROPERTY STRINGS hw_raspi3b hw_raspi4b qemu_raspi3b qemu_virt)
set(KORAOS_USERFS_DIR "" CACHE PATH "Prepared shared AArch64 userfs directory")
set(RPI_VERSION "4" CACHE STRING "Physical Raspberry Pi version (3 or 4)")
set_property(CACHE RPI_VERSION PROPERTY STRINGS 3 4)
option(BUILD_QEMU_VARIANT "Build kernel image configured for QEMU" ON)
option(BUILD_VIRT_VARIANT "Build kernel for QEMU AArch64 virt" OFF)
option(BUILD_HW_VARIANT "Build kernel image configured for physical hardware" ON)

if(KORAOS_TARGET)
    set(BUILD_QEMU_VARIANT OFF)
    set(BUILD_VIRT_VARIANT OFF)
    set(BUILD_HW_VARIANT OFF)
    if(KORAOS_TARGET STREQUAL "qemu_raspi3b")
        set(BUILD_QEMU_VARIANT ON)
        set(RPI_VERSION 3)
    elseif(KORAOS_TARGET STREQUAL "qemu_virt")
        set(BUILD_VIRT_VARIANT ON)
        set(RPI_VERSION 3)
    elseif(KORAOS_TARGET STREQUAL "hw_raspi3b")
        set(BUILD_HW_VARIANT ON)
        set(RPI_VERSION 3)
    elseif(KORAOS_TARGET STREQUAL "hw_raspi4b")
        set(BUILD_HW_VARIANT ON)
        set(RPI_VERSION 4)
    else()
        message(FATAL_ERROR "Unknown KORAOS_TARGET '${KORAOS_TARGET}'; choose hw_raspi3b, hw_raspi4b, qemu_raspi3b or qemu_virt.")
    endif()
    set(_default_bootmnt "${CMAKE_BINARY_DIR}/boot")
else()
    set(_default_bootmnt "/workspace/build/boot")
endif()
if(NOT RPI_VERSION MATCHES "^[34]$")
    message(FATAL_ERROR "RPI_VERSION must be 3 or 4; the raspi3b QEMU target always uses Pi 3.")
endif()
set(BOOTMNT "${_default_bootmnt}" CACHE PATH "Boot mount directory")

# Directories
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
set(INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(LINKER_SCRIPT "${SRC_DIR}/linker.ld")

# Raspberry Pi GPU boot firmware + device trees (third-party, see firmware/).
# Staged next to the kernel on hardware boot partitions. bootcode.bin/start.elf/
# fixup.dat boot the Pi 3; start4.elf/fixup4.dat boot the Pi 4 (which has no
# bootcode.bin). Both DTBs ship so one card serves either board.
set(BOOT_FIRMWARE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/LICENCE.broadcom"
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bootcode.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/start.elf"
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/fixup.dat"
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/start4.elf"
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/fixup4.dat"
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bcm2710-rpi-3-b.dtb"
    "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bcm2711-rpi-4-b.dtb"
)

# Gather sources from src tree (including subdirectories) and src/lib. C++ lives
# alongside C (e.g. src/cxx/ and, later, the vendored Circle USB stack); the
# kernel and its C++ share one flat image.
file(GLOB_RECURSE KERNEL_C_SOURCES "${SRC_DIR}/*.c")
file(GLOB_RECURSE KERNEL_CXX_SOURCES "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.cc" "${SRC_DIR}/*.cxx")
file(GLOB_RECURSE KERNEL_ASM_SOURCES "${SRC_DIR}/*.S" "${SRC_DIR}/*.s")
# The Circle HAL-bridge adapters (src/circle) need Circle's headers and build
# flags, so they are compiled into the circle_usb library below, not with the
# kernel's own C++ flags.
list(FILTER KERNEL_CXX_SOURCES EXCLUDE REGEX "/circle/")
file(GLOB LIB_C_SOURCES "${LIB_DIR}/*.c")
file(GLOB LIB_ASM_SOURCES "${LIB_DIR}/*.S" "${LIB_DIR}/*.s")

set(KERNEL_SOURCES ${KERNEL_C_SOURCES} ${KERNEL_CXX_SOURCES} ${KERNEL_ASM_SOURCES} ${LIB_C_SOURCES} ${LIB_ASM_SOURCES})

# Canonical kernel configurations consume the already prepared immutable
# image; only the standalone userfs project is a producer for modern builds.
if(KORAOS_TARGET)
    if(NOT KORAOS_USERFS_DIR)
        message(FATAL_ERROR "KORAOS_USERFS_DIR is required; build cmake/userfs first.")
    endif()
    get_filename_component(_userfs_dir "${KORAOS_USERFS_DIR}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(FS_IMAGE "${_userfs_dir}/koraos.img")
    if(NOT EXISTS "${FS_IMAGE}" OR IS_DIRECTORY "${FS_IMAGE}")
        message(FATAL_ERROR "Prepared userfs image not found: ${FS_IMAGE}; build cmake/userfs first.")
    endif()
    add_custom_target(filesystem_image DEPENDS "${FS_IMAGE}")
else()
    # Compatibility with old direct multi-variant CMake builds.
    set(FS_IMAGE "${CMAKE_BINARY_DIR}/fs/koraos.img")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/userfs.cmake")
    koraos_add_userfs("${CMAKE_CURRENT_SOURCE_DIR}" "${FS_IMAGE}")
endif()

set(FS_BLOB_ASM "${CMAKE_BINARY_DIR}/fs/fs_blob.S")
file(GENERATE OUTPUT "${FS_BLOB_ASM}" CONTENT
"// Generated by CMake. Do not edit. Embeds the FAT32 ramdisk image.\n\n.section .rodata.koraosfs,\"a\"\n.balign 8\n.global _koraos_fs_start\n_koraos_fs_start:\n    .incbin \"${FS_IMAGE}\"\n.global _koraos_fs_end\n_koraos_fs_end:\n")

# Reassemble the blob (and so relink) whenever the image changes, and make sure
# the image is produced before the blob is assembled.
set_source_files_properties("${FS_BLOB_ASM}" PROPERTIES
    GENERATED TRUE
    OBJECT_DEPENDS "${FS_IMAGE}"
)

list(APPEND KERNEL_SOURCES "${FS_BLOB_ASM}")

# The build identifier (git commit) printed at boot: regenerated on every
# build, rewritten only when it changes (see cmake/build-id.cmake).
set(BUILD_ID_SOURCE "${CMAKE_BINARY_DIR}/generated/build_id.c")
add_custom_target(build_id
    COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOUTPUT=${BUILD_ID_SOURCE} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-id.cmake"
    BYPRODUCTS "${BUILD_ID_SOURCE}"
    COMMENT "Checking the build identifier"
    VERBATIM)
list(APPEND KERNEL_SOURCES "${BUILD_ID_SOURCE}")

# Common compiler flags
set(COMMON_FLAGS
    --target=aarch64-none-elf
    -mcpu=cortex-a72
    -ffreestanding
    -nostdlib
    -fno-builtin
    -fno-stack-protector
)

# C-specific flags
set(C_FLAGS
    ${COMMON_FLAGS}
    -Wall
    -mgeneral-regs-only
    -DPRINTF_LONG_SUPPORT
)

# C++-specific flags. Freestanding kernel C++: no exceptions, no RTTI, no
# thread-safe local-static guards (the kernel is single-threaded here), and
# integer-only codegen like the C side. Static-object destructors are routed to
# the __cxa_atexit stub in src/cxx/ and never run (the kernel does not exit).
set(CXX_FLAGS
    ${COMMON_FLAGS}
    -Wall
    -mgeneral-regs-only
    -std=c++20
    -nostdinc++
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -DPRINTF_LONG_SUPPORT
)

# Assembly flags
set(ASM_FLAGS
    ${COMMON_FLAGS}
)

include(CMakeParseArguments)

if(NOT OBJCOPY)
    find_program(OBJCOPY llvm-objcopy)
endif()

if(NOT OBJCOPY)
    message(FATAL_ERROR "llvm-objcopy not found. Install the LLVM toolchain or provide OBJCOPY explicitly.")
endif()

function(add_kernel_variant VARIANT)
    set(options QEMU VIRT)
    set(oneValueArgs BOARD OUTPUT_NAME IMAGE_NAME BOOT_IMAGE_NAME MAP_NAME)
    cmake_parse_arguments(KVAR "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT KVAR_OUTPUT_NAME)
        set(KVAR_OUTPUT_NAME ${VARIANT})
    endif()

    if(NOT KVAR_IMAGE_NAME)
        set(KVAR_IMAGE_NAME "${KVAR_OUTPUT_NAME}.img")
    endif()

    if(NOT KVAR_BOOT_IMAGE_NAME)
        set(KVAR_BOOT_IMAGE_NAME ${KVAR_IMAGE_NAME})
    endif()

    if(NOT KVAR_MAP_NAME)
        set(KVAR_MAP_NAME "${KVAR_OUTPUT_NAME}.map")
    endif()

    if(KORAOS_TARGET)
        set(KVAR_OUTPUT_NAME kernel)
        set(KVAR_IMAGE_NAME kernel.img)
        set(KVAR_MAP_NAME kernel.map)
    endif()

    set(target "kernel_${VARIANT}")
    set(variant_sources ${KERNEL_SOURCES})
    if(KVAR_VIRT)
        list(FILTER variant_sources EXCLUDE REGEX "/(gpio|mailbox)\\.c$")
    else()
        list(FILTER variant_sources EXCLUDE REGEX "/(platform/|drivers/virtio)")
    endif()
    add_executable(${target} ${variant_sources})
    add_dependencies(${target} filesystem_image build_id)

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${KVAR_OUTPUT_NAME}
        SUFFIX ".elf"
        LINK_DEPENDS ${LINKER_SCRIPT}
    )

    target_include_directories(${target} PRIVATE ${INCLUDE_DIR})

    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:${C_FLAGS}>
        $<$<COMPILE_LANGUAGE:CXX>:${CXX_FLAGS}>
        $<$<COMPILE_LANGUAGE:ASM>:${ASM_FLAGS}>
    )

    if(NOT KVAR_BOARD MATCHES "^[34]$")
        message(FATAL_ERROR "Kernel variant ${VARIANT} requires BOARD 3 or 4.")
    endif()
    target_compile_definitions(${target} PRIVATE RPI_VERSION=${KVAR_BOARD})
    if(KVAR_VIRT)
        target_compile_definitions(${target} PRIVATE KORAOS_VIRT KORAOS_RAMFB KORAOS_VIRTIO_BLK KORAOS_VIRTIO_INPUT)
        target_link_options(${target} PRIVATE --defsym=KERNEL_LOAD_ADDRESS=0x40080000)
    endif()
    if(KVAR_QEMU)
        target_compile_definitions(${target} PRIVATE QEMU_TESTING)
    endif()

    target_link_options(${target} PRIVATE
        -T${LINKER_SCRIPT}
        -Map=${CMAKE_BINARY_DIR}/${KVAR_MAP_NAME}
    )

    set(image_path "${CMAKE_BINARY_DIR}/${KVAR_IMAGE_NAME}")

    if(KORAOS_TARGET)
        # An explicit output restores a deleted image without recompiling the
        # kernel, and makes hardware installation depend on image generation.
        add_custom_command(OUTPUT "${image_path}"
            COMMAND ${OBJCOPY} -O binary $<TARGET_FILE:${target}> "${image_path}"
            DEPENDS ${target}
            COMMENT "Creating ${KVAR_IMAGE_NAME} binary"
            VERBATIM)
        add_custom_target(image_${VARIANT} ALL DEPENDS "${image_path}")
        set(image_dependency image_${VARIANT})
    else()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${OBJCOPY} -O binary $<TARGET_FILE:${target}> "${image_path}"
            COMMENT "Creating ${KVAR_IMAGE_NAME} binary"
            VERBATIM
            BYPRODUCTS "${image_path}")
        set(image_dependency ${target})
    endif()

    if(NOT KORAOS_TARGET)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/boot
            COMMAND ${CMAKE_COMMAND} -E copy ${image_path} ${CMAKE_BINARY_DIR}/boot/${KVAR_BOOT_IMAGE_NAME}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config.txt ${CMAKE_BINARY_DIR}/boot/config.txt
            COMMENT "Copying ${KVAR_IMAGE_NAME} to local boot directory (${KVAR_BOOT_IMAGE_NAME})"
            VERBATIM)
        if(NOT KVAR_QEMU AND NOT KVAR_VIRT)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${BOOT_FIRMWARE_FILES} ${CMAKE_BINARY_DIR}/boot/
                COMMENT "Staging Raspberry Pi boot firmware into local boot directory"
                VERBATIM)
        endif()
    endif()

    # Canonical builds install hardware only, on explicit request. Compatibility
    # configurations retain all of their existing install targets.
    if(NOT KORAOS_TARGET OR (NOT KVAR_QEMU AND NOT KVAR_VIRT))
        add_custom_target(install_${VARIANT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${BOOTMNT}
            COMMAND ${CMAKE_COMMAND} -E copy ${image_path} ${BOOTMNT}/${KVAR_BOOT_IMAGE_NAME}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config.txt ${BOOTMNT}/config.txt
            DEPENDS ${image_dependency}
            COMMENT "Copying ${KVAR_IMAGE_NAME} to boot mount: ${BOOTMNT}"
            VERBATIM)
        if(NOT KVAR_QEMU AND NOT KVAR_VIRT)
            add_custom_command(TARGET install_${VARIANT} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${BOOT_FIRMWARE_FILES} ${BOOTMNT}/
                COMMENT "Copying Raspberry Pi boot firmware to boot mount: ${BOOTMNT}"
                VERBATIM)
        endif()
        set_property(GLOBAL APPEND PROPERTY KERNEL_INSTALL_TARGETS install_${VARIANT})
    endif()
    set_property(GLOBAL APPEND PROPERTY KERNEL_VARIANT_TARGETS ${target})
endfunction()

if(BUILD_QEMU_VARIANT)
    add_kernel_variant(qemu
        QEMU BOARD 3
        OUTPUT_NAME kernel8
        IMAGE_NAME kernel8.img
        BOOT_IMAGE_NAME kernel8-qemu.img
        MAP_NAME kernel8.map
    )
endif()

if(BUILD_VIRT_VARIANT)
    add_kernel_variant(virt QEMU VIRT BOARD 3
        OUTPUT_NAME kernel-virt
        IMAGE_NAME kernel-virt.img
        BOOT_IMAGE_NAME kernel-virt.img
        MAP_NAME kernel-virt.map)
endif()

if(BUILD_HW_VARIANT)
    add_kernel_variant(hw
        BOARD ${RPI_VERSION}
        OUTPUT_NAME kernel8-hw
        IMAGE_NAME kernel8-hw.img
        BOOT_IMAGE_NAME kernel8-rpi${RPI_VERSION}.img
        MAP_NAME kernel8-hw.map
    )
endif()

get_property(_kernel_targets GLOBAL PROPERTY KERNEL_VARIANT_TARGETS)
if(NOT _kernel_targets)
    message(FATAL_ERROR "No kernel variants are enabled. Enable at least one of BUILD_QEMU_VARIANT or BUILD_HW_VARIANT or BUILD_VIRT_VARIANT.")
endif()

get_property(_kernel_install_targets GLOBAL PROPERTY KERNEL_INSTALL_TARGETS)
if(_kernel_install_targets)
    add_custom_target(install_kernel DEPENDS ${_kernel_install_targets})
endif()

set(enabled_variants)
if(BUILD_QEMU_VARIANT)
    list(APPEND enabled_variants "QEMU")
endif()
if(BUILD_VIRT_VARIANT)
    list(APPEND enabled_variants "QEMU virt")
endif()
if(BUILD_HW_VARIANT)
    list(APPEND enabled_variants "hardware")
endif()
list(JOIN enabled_variants ", " enabled_variants_str)

# ---------------------------------------------------------------------------
# Vendored Circle USB stack (third_party/circle, pinned fork kora-os/circle),
# built as a static library together with the KoraOS HAL-bridge adapters in
# src/circle. Circle is compiled for the board being built (RASPPI), which picks
# the host controller behind CUSBHCIDevice: DWC2 on the Pi 3, xHCI (the VL805
# behind PCIe) on the Pi 4. Circle's USB core is compiled for one controller
# family or the other, so each board's build leaves the other's sources out:
# the Pi 3 skips xHCI/PCIe/device tree, the Pi 4 skips DWC2 (so its USB-C port,
# a DWC2, is not supported; it is normally the power input anyway).
# ---------------------------------------------------------------------------
set(CIRCLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/circle")
file(GLOB_RECURSE CIRCLE_ALL_SOURCES "${CIRCLE_DIR}/lib/*.cpp")
# Each board needs its own controller sources and RASPPI definition. Sharing
# one library would couple the retained raspi3b target to the hardware board.
file(GLOB CIRCLE_BRIDGE_SOURCES "${SRC_DIR}/circle/*.cpp")
set(KORAOS_KEYMAP "US" CACHE STRING "USB keyboard layout: US, UK, DE, FR, IT, ES or DV")
function(add_circle_library NAME BOARD)
    set(circle_sources ${CIRCLE_ALL_SOURCES})
    if(BOARD LESS 4)
        list(FILTER circle_sources EXCLUDE REGEX "/(usb/xhci[a-z]*|bcmpciehostbridge|devicetreeblob)\\.cpp$")
    else()
        list(FILTER circle_sources EXCLUDE REGEX "/usb/dwhci[a-z]*\\.cpp$")
    endif()
    add_library(${NAME} STATIC ${circle_sources} ${CIRCLE_BRIDGE_SOURCES})
    target_include_directories(${NAME} PRIVATE "${CIRCLE_DIR}/include" "${INCLUDE_DIR}")
    target_compile_definitions(${NAME} PRIVATE
        RASPPI=${BOARD} RPI_VERSION=${BOARD} DEFAULT_KEYMAP="${KORAOS_KEYMAP}")
    target_compile_options(${NAME} PRIVATE
        --target=aarch64-none-elf
        -mcpu=cortex-a72
        -ffreestanding
        -nostdlib
        -fno-builtin
        # Integer-only, like the rest of the kernel: FP/SIMD registers belong to
        # EL0 tasks (lazily context switched), and Circle runs in interrupt
        # handlers that may interrupt any task. KORAOS_INTEGER_ONLY drops
        # CString's %f path, the one use of floating point (see PROVENANCE.md).
        -mgeneral-regs-only
        -DKORAOS_INTEGER_ONLY
        -nostdinc++
        -fno-exceptions
        -fno-rtti
        -fno-threadsafe-statics
        -std=c++17
        -DAARCH=64
        # No C standard library underneath Circle here.
        -DSTDLIB_SUPPORT=0
        # Build only the HID keyboard device driver from Circle's USB device factory;
        # every other class (mouse, gamepads, storage, audio, net, ...) is excluded so
        # we do not have to vendor those drivers and their dependencies.
        -DEXCLUDE_USB_AUDIO
        -DEXCLUDE_USB_BLUETOOTH
        -DEXCLUDE_USB_GAMEPAD
        -DEXCLUDE_USB_MIDI
        -DEXCLUDE_USB_MOUSE
        -DEXCLUDE_USB_NET
        -DEXCLUDE_USB_PRINTER
        -DEXCLUDE_USB_SERIAL
        -DEXCLUDE_USB_STORAGE
        -DEXCLUDE_USB_TOUCHSCREEN
        # Vendored third-party code: do not enforce KoraOS's warning policy on it.
        -Wno-everything
    )
endfunction()

if(BUILD_QEMU_VARIANT OR (BUILD_HW_VARIANT AND RPI_VERSION STREQUAL "3"))
    add_circle_library(circle_usb_rpi3 3)
endif()
if(BUILD_HW_VARIANT AND RPI_VERSION STREQUAL "4")
    add_circle_library(circle_usb_rpi4 4)
endif()
if(BUILD_QEMU_VARIANT)
    target_link_libraries(kernel_qemu PRIVATE circle_usb_rpi3)
endif()
if(BUILD_HW_VARIANT)
    target_link_libraries(kernel_hw PRIVATE circle_usb_rpi${RPI_VERSION})
endif()
# QEMU virt uses independent VirtIO input and never links Circle.

# Print configuration
message(STATUS "Physical hardware board: Raspberry Pi ${RPI_VERSION}; QEMU raspi3b board: Pi 3")
message(STATUS "Boot mount directory: ${BOOTMNT}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "Linker script: ${LINKER_SCRIPT}")
message(STATUS "Kernel variants enabled: ${enabled_variants_str}")
