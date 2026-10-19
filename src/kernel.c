#include "arch/cxx.h"
#include "arch/exception.h"
#include "arch/irq.h"
#include "arch/percpu.h"
#include "arch/smp.h"
#include "arch/systick.h"
#ifdef KORAOS_VIRT
#include "platform/virt.h"
#include "drivers/virtio_input.h"
#else
#include "circle_env.h"
#endif
#include "console.h"
#include "fs/blkdev.h"
#include "fs/fat32.h"
#include "fs/namespace.h"
#include "mm.h"
#include "mm/frame_alloc.h"
#include "mm/kmalloc.h"
#include "mm/mmu.h"
#include "proc/task.h"
#include "lib/panic.h"
#include "lib/printf.h"
#include "lib/stdlib.h"
#include "mini_uart.h"
#include "tty.h"
#include "utils.h"
#include "video/console_fb.h"

void putc(void *p, char c) {
  if (c == '\n') {
    uart_putc('\r');
  }

  uart_putc(c);  // kernel log is UART-only; the screen belongs to the tty
}

void kernel_main(uintptr_t dtb) {
  // Per-CPU data first: spinlocks and the scheduler find this core through it.
  // No spinlock may be taken before mmu_init() below (see src/arch/spinlock.c).
  percpu_init(0);

#ifdef KORAOS_VIRT
  if (!virt_platform_init(dtb)) {
    for (;;) { asm volatile("wfi"); }
  }
#else
  (void)dtb;
#endif
  uart_init();
  // First words on the UART: which kernel this is, down to the commit, so a
  // log always shows whether the board runs the build you think it does.
  uart_puts("KoraOS " KORAOS_VERSION " (");
  uart_puts(koraos_build_id);
  uart_puts(")\n");

  init_printf(NULL, putc);  // unlocked until printf_lock_init() below

  // Run C++ global constructors now that printf is available. (Constructors
  // must not allocate yet: the frame allocator is brought up further down.)
  cxx_init();

  // Install EL1 exception vectors before doing anything that could trap.
  exception_init();

  // Enable the MMU with a flat, fully-permissive identity map, then bring up
  // the physical page allocator for later user-stack allocation.
  mmu_init();

  // Caches are on: spinlocks work. kernel_main becomes task 0, holding the
  // big kernel lock, and printf starts taking its lock.
  task_init_boot();
  printf_lock_init();

  frame_alloc_init();

  // Exercise the kernel heap once before anything depends on it.
  int heap_rc = kmalloc_stress(2000, 1);
  if (heap_rc != 0) {
    printf("[heap] self-test FAILED (check %d)\n", heap_rc);
  } else {
    printf("[heap] self-test ok, %u of %u pages free\n",
           (unsigned)frame_alloc_free_count(), (unsigned)frame_alloc_total_count());
  }

  // Prove the freestanding C++ toolchain and runtime work end to end (static
  // ctors, virtual dispatch, operator new via the kernel heap). This is
  // scaffolding for the Circle USB stack; remove once real C++ drivers land.
  cxx_selftest();

  // Bring up KoraOS's interrupt controller, start the 100 Hz system tick, and
  // unmask IRQs. This is KoraOS's own interrupt layer; the vendored USB stack
  // is bridged onto it in a later step.
  irq_init();
  systick_init(100);
  irq_enable();

  // Bring up the other cores: each turns its MMU on, sets up its own
  // interrupt controller interface and tick, and idles. Device interrupts and
  // Circle stay on this core.
  smp_start_secondaries();

  // Bring up the vendored Circle USB stack on the KoraOS HAL bridge. Enumeration
  // talks to real USB hardware, which QEMU's raspi3b does not emulate, so only
  // the hardware build initializes and scans for a keyboard.
#ifndef KORAOS_VIRT
#ifdef QEMU_TESTING
  circle_usb_init(0);
#else
  circle_usb_init(1);
#endif
#endif

  // Bring up the ramdisk block device (embedded FAT32 image) and mount it so
  // the file syscalls have a filesystem to serve.
  blkdev_init();
  int fs_rc = fs_mount_registered(blkdev_root());
  if (fs_rc != 0) {
    printf("fat32: mount failed: %d\n", fs_rc);
  }

  // Persist the screen console for the lifetime of the kernel and make it the
  // active screen, so printf output and the write/fb_info syscalls reach it.
  static fb_console_t fb_console;
  if (!fb_console_init(&fb_console, 1024, 768, 32)) {
    printf("video: framebuffer init failed (no HDMI output)\n");
  } else {
    printf("video: framebuffer %ux%ux%u pitch %u at 0x%lx\n", fb_console.fb.width,
           fb_console.fb.height, fb_console.fb.depth, fb_console.fb.pitch,
           (unsigned long)(uintptr_t)fb_console.fb.buffer);
    fb_console_make_active(&fb_console);
    fb_console_write(&fb_console, "KoraOS\n");
    fb_console_write(&fb_console, "Hello from framebuffer console.\n");
  }

#ifdef KORAOS_VIRT
  console_log("KoraOS is running on QEMU virt!\n");
#elif RPI_VERSION == 4
#if QEMU_TESTING
  console_log("KoraOS is running on a Raspberry Pi 4 in QEMU!\n");
#else
  console_log("KoraOS is running on a Raspberry Pi 4!\n");
#endif
#else
#if QEMU_TESTING
  console_log("KoraOS is running on a Raspberry Pi 3 in QEMU!\n");
#else
  console_log("KoraOS is running on a Raspberry Pi 3!\n");
#endif
#endif

  printf("Current EL: %d\n", get_el());

  // The kernel debug console lives on the UART alongside the shell on the
  // screen: fed from the UART interrupt (and the terminal's idle loop), not a
  // task of its own. Hooked only now so it does not interleave with boot output.
  console_init();
  tty_serial_init();
#ifdef KORAOS_VIRTIO_INPUT
  printf("[virtio-input] keyboard %s\n", virtio_input_init() ? "ready" : "absent; serial input available");
#endif

  // Start /bin/init as the first (and only) user program the kernel launches.
  // init owns userland policy from here: it spawns the shell, which spawns
  // further programs -- all loaded from the filesystem.
  int pid = task_spawn("init", 0, 0, 0);
  if (pid < 0) {
    printf("kernel: failed to load /bin/init\n");
  } else {
    int code = task_wait(pid);  // reap init (its parent is the kernel)
    printf("init (pid %d) exited with code %d\n", pid, code);
  }
  task_reap_all();  // release anything left unreaped

  // Nothing left to run: keep serving the serial line (the debug console, or
  // Ctrl-T to switch) for as long as the machine is up, without the BKL.
  bkl_release_for_good();
  for (;;) {
    tty_poll_serial();
  }
}
