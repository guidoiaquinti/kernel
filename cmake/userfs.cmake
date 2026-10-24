# Shared AArch64 userland/FAT32 producer used by the standalone project and
# legacy direct kernel configurations. Modern kernels only consume its image.
include_guard(GLOBAL)

function(koraos_add_user_program NAME)
    set(elf "${USER_BUILD_DIR}/${NAME}.elf")
    add_custom_command(
        OUTPUT "${elf}"
        COMMAND ${CMAKE_C_COMPILER} ${USER_COMPILE_FLAGS} ${USER_LINK_FLAGS}
                ${USER_RUNTIME_SOURCES} ${ARGN} -o "${elf}"
        DEPENDS ${USER_RUNTIME_SOURCES} ${ARGN} ${USER_HEADERS} "${USER_HEADER_MANIFEST}"
                "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        COMMENT "Building user program ${NAME}.elf (PIE)"
        VERBATIM)
    set(USER_PROGRAM_ELVES ${USER_PROGRAM_ELVES} "${elf}" PARENT_SCOPE)
endfunction()

function(koraos_add_userfs SOURCE_ROOT IMAGE_PATH)
    set(USER_DIR "${SOURCE_ROOT}/user")
    set(USER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/user")
    file(MAKE_DIRECTORY "${USER_BUILD_DIR}")
    set(USER_RUNTIME_SOURCES "${USER_DIR}/crt0.S" "${USER_DIR}/libk/syscall.S"
        "${USER_DIR}/libk/malloc.c" "${USER_DIR}/libk/mem.c" "${USER_DIR}/libk/mutex.c"
        "${USER_DIR}/libk/job.c")
    file(GLOB_RECURSE USER_HEADERS CONFIGURE_DEPENDS "${USER_DIR}/*.h")
    # Manifest content changes on removals too, ensuring generated outputs are
    # rebuilt even when the remaining dependencies are older than those outputs.
    set(USER_HEADER_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/userfs-headers.txt")
    string(JOIN "\n" _headers ${USER_HEADERS})
    file(GENERATE OUTPUT "${USER_HEADER_MANIFEST}" CONTENT "${_headers}\n")
    set(USER_COMPILE_FLAGS
        --target=aarch64-none-elf -mcpu=cortex-a72 -ffreestanding -nostdlib
        -fno-builtin -fno-stack-protector -mgeneral-regs-only -fPIE -O2 -Wall
        "-I${USER_DIR}")
    # Preserve ET_DYN, classic DT_RELA relocations and 4 KiB segment alignment.
    set(USER_LINK_FLAGS
        -nostdlib -fuse-ld=lld -Wl,-pie -Wl,-e,_start -Wl,--pack-dyn-relocs=none
        -Wl,-z,norelro -Wl,-z,noexecstack -Wl,-z,max-page-size=4096)
    set(USER_PROGRAM_ELVES "")
    foreach(name hello init shell echo gfxdemo termdemo ls cat)
        koraos_add_user_program(${name} "${USER_DIR}/${name}.c")
    endforeach()
    foreach(name cp rm mkdir rmdir mv ps jobdemo pindemo)
        koraos_add_user_program(${name} "${USER_DIR}/${name}.c")
    endforeach()
    koraos_add_user_program(allocprobe "${SOURCE_ROOT}/tests/user/allocprobe.c")
    koraos_add_user_program(fpprobe "${SOURCE_ROOT}/tests/user/fpprobe.c"
        "${SOURCE_ROOT}/tests/user/fpregs.S")
    koraos_add_user_program(schedprobe "${SOURCE_ROOT}/tests/user/schedprobe.c")
    koraos_add_user_program(forbidprobe "${SOURCE_ROOT}/tests/user/forbidprobe.c")
    koraos_add_user_program(threadprobe "${SOURCE_ROOT}/tests/user/threadprobe.c")
    koraos_add_user_program(sumprobe "${SOURCE_ROOT}/tests/user/sumprobe.c")
    koraos_add_user_program(nsprobe "${SOURCE_ROOT}/tests/user/nsprobe.c")
    koraos_add_user_program(writeprobe "${SOURCE_ROOT}/tests/user/writeprobe.c")

    # Retain the previous --bindir glob installation order in /bin.
    list(SORT USER_PROGRAM_ELVES)
    set(USER_PROGRAM_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/userfs-programs.txt")
    string(JOIN "\n" _programs ${USER_PROGRAM_ELVES})
    file(GENERATE OUTPUT "${USER_PROGRAM_MANIFEST}" CONTENT "${_programs}\n")

    set(FS_ROOT_DIR "${SOURCE_ROOT}/fsroot")
    set(FS_IMAGE_SCRIPT "${SOURCE_ROOT}/create-fs-image.sh")
    file(GLOB_RECURSE FS_ROOT_FILES LIST_DIRECTORIES true CONFIGURE_DEPENDS "${FS_ROOT_DIR}/*")
    set(FS_INPUT_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/userfs-root-files.txt")
    string(JOIN "\n" _root_files ${FS_ROOT_FILES})
    file(GENERATE OUTPUT "${FS_INPUT_MANIFEST}" CONTENT "${_root_files}\n")
    add_custom_command(
        OUTPUT "${IMAGE_PATH}"
        COMMAND "${FS_IMAGE_SCRIPT}" --fsroot "${FS_ROOT_DIR}" --output "${IMAGE_PATH}"
                --programs-file "${USER_PROGRAM_MANIFEST}"
        DEPENDS ${FS_ROOT_FILES} ${USER_PROGRAM_ELVES} "${FS_IMAGE_SCRIPT}" "${FS_INPUT_MANIFEST}"
                "${USER_PROGRAM_MANIFEST}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        COMMENT "Building FAT32 ramdisk image koraos.img"
        VERBATIM)
    # Single dependency target prevents parallel kernel variants from racing to
    # rebuild the same image in compatibility configurations.
    add_custom_target(filesystem_image DEPENDS "${IMAGE_PATH}")
    add_custom_target(userfs ALL DEPENDS filesystem_image)
endfunction()
