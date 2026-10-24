// KoraOS userland syscall stubs. AAPCS already puts the C arguments in x0..x3,
// so each stub only loads the syscall number into x8 and issues `svc #0`; the
// return value is already in x0 on the way back.

#include "abi.h"

.section .text

.macro SYSCALL name, num
.global \name
\name:
    mov x8, #\num
    svc #0
    ret
.endm

SYSCALL write,   SYS_WRITE
SYSCALL read,    SYS_READ
SYSCALL spawn_flags, SYS_SPAWN_FLAGS
SYSCALL waitpid, SYS_WAITPID
SYSCALL getpid,  SYS_GETPID
SYSCALL yield,   SYS_YIELD
SYSCALL fb_info, SYS_FB_INFO
SYSCALL open,    SYS_OPEN
SYSCALL close,   SYS_CLOSE
SYSCALL lseek,   SYS_LSEEK
SYSCALL readdir, SYS_READDIR
SYSCALL stat,    SYS_STAT
SYSCALL alloc_pages, SYS_ALLOC_PAGES
SYSCALL free_pages,  SYS_FREE_PAGES
SYSCALL msleep,  SYS_MSLEEP
SYSCALL forbid,  SYS_FORBID
SYSCALL permit,  SYS_PERMIT
SYSCALL task_info, SYS_TASK_INFO
SYSCALL thread_new,  SYS_THREAD_NEW
SYSCALL thread_exit, SYS_THREAD_EXIT
SYSCALL thread_join, SYS_THREAD_JOIN
SYSCALL kill,        SYS_KILL
SYSCALL sysinfo,     SYS_SYSINFO
SYSCALL chdir,   SYS_CHDIR
SYSCALL getcwd,  SYS_GETCWD
SYSCALL volume_info, SYS_VOLUME_INFO
SYSCALL assign,      SYS_ASSIGN
SYSCALL assign_info, SYS_ASSIGN_INFO
SYSCALL sync,        SYS_SYNC
SYSCALL unlink,      SYS_UNLINK
SYSCALL mkdir,       SYS_MKDIR
SYSCALL rmdir,       SYS_RMDIR
SYSCALL rename,      SYS_RENAME

// exit() never returns; if the kernel ever did return here, spin rather than
// running off into whatever follows.
.global exit
exit:
    mov x8, #SYS_EXIT
    svc #0
1:  b 1b

// Every thread_create() thread starts here, with x0 = fn and x1 = arg (see
// thread_create in koraos.h): it runs fn(arg) and passes its result to
// thread_exit(), which does not return.
.global libk_thread_start
libk_thread_start:
    mov x2, x0
    mov x0, x1
    blr x2
    mov x8, #SYS_THREAD_EXIT
    svc #0
1:  b 1b
