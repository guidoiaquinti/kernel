// SPDX-License-Identifier: GPL-3.0-or-later
//
// User-facing terminal: screen output, keyboard (+ UART fallback) input. See
// tty.h.

#include "tty.h"

#include "arch/irq.h"
#include "console.h"
#include "arch/spinlock.h"
#include "mini_uart.h"
#include "proc/task.h"
#include "video/console_fb.h"

// Input queue, filled by keyboard and UART interrupt handlers (and terminal
// replies) and drained by tasks reading the console. input_lock (IRQ-safe)
// covers the queue, scroll_request and the wait queue's condition.
#define INPUT_QUEUE_SIZE 256  // power of two

static struct spinlock input_lock = SPINLOCK_INIT("tty input");
static char input_queue[INPUT_QUEUE_SIZE];
static unsigned input_head;
static unsigned input_tail;

// Readers sleep here until input (or a scrollback request) arrives.
static struct wait_queue input_waiters;

void tty_input_push(char c) {
    uint64_t flags = spin_lock_irqsave(&input_lock);
    unsigned head = input_head;
    if (head - input_tail != INPUT_QUEUE_SIZE) {  // full: drop
        input_queue[head % INPUT_QUEUE_SIZE] = c;
        input_head = head + 1;
        wait_queue_wake_all(&input_waiters);
    }
    spin_unlock_irqrestore(&input_lock, flags);
}

// Called with input_lock held.
static int input_pop(char *c) {
    unsigned tail = input_tail;
    if (tail == input_head) {
        return 0;
    }
    *c = input_queue[tail % INPUT_QUEUE_SIZE];
    input_tail = tail + 1;
    return 1;
}

// Serial input routing. The UART belongs either to the kernel debug console
// (console.c) or, as a fallback keyboard, to this terminal; Ctrl-T switches.
#define SERIAL_SWITCH_KEY 0x14  // Ctrl-T

#ifdef QEMU_TESTING
static bool serial_to_tty = true;   // no USB keyboard: the UART drives the shell
#else
static bool serial_to_tty = false;  // the UART is the kernel debug console
#endif

static void announce_serial_route(void) {
    if (serial_to_tty) {
        uart_puts("\n[serial -> screen terminal; Ctrl-T for the kernel console]\n");
    } else {
        uart_puts("\n[serial -> kernel console; Ctrl-T for the screen terminal]\n");
        console_prompt();
    }
}

// serial_lock (IRQ-safe) keeps the UART interrupt and the boot thread's
// polling loop from draining the UART, or flipping the route, at once. The
// debug console's commands run under it (lock order: serial, then the others).
static struct spinlock serial_lock = SPINLOCK_INIT("serial");

void tty_poll_serial(void) {
    uint64_t flags = spin_lock_irqsave(&serial_lock);
    while (uart_rx_ready()) {
        char c = (char)uart_getc();
        if (c == SERIAL_SWITCH_KEY) {
            serial_to_tty = !serial_to_tty;
            announce_serial_route();
        } else if (serial_to_tty) {
            tty_input_push(c);
        } else {
            console_input(c);
        }
    }
    spin_unlock_irqrestore(&serial_lock, flags);
}

static void serial_isr(void *ctx) {
    (void)ctx;
    tty_poll_serial();
}

void tty_serial_init(void) {
    announce_serial_route();
    uart_rx_irq_enable(serial_isr);
}

// Scrollback requests from the keyboard (IRQ context), in half screens. They
// are applied by the task waiting for input (see tty_getc), so the screen is
// never drawn from an interrupt handler.
static int scroll_request;

void tty_scrollback(int halfpages) {
    uint64_t flags = spin_lock_irqsave(&input_lock);
    scroll_request += halfpages;
    wait_queue_wake_all(&input_waiters);
    spin_unlock_irqrestore(&input_lock, flags);
}

// Called with input_lock held; returns and clears the pending request.
static int take_scroll_request(void) {
    int request = scroll_request;
    scroll_request = 0;
    return request;
}

void tty_flush(void) {
    screen_flush();
}

int tty_getc(void) {
    tty_flush();  // whatever was echoed or printed must be visible while we wait

    // Sleep until a keyboard (USB, VirtIO) or UART interrupt pushes input.
    // The queue is checked and the sleep entered under input_lock, so a byte
    // arriving in between still wakes us; other tasks run meanwhile, and with
    // nothing runnable the scheduler idles with IRQs open.
    // Scrollback is drawn outside the lock: the screen belongs to tasks (the
    // BKL), not to the interrupt handlers input_lock fends off.
    uint64_t flags = spin_lock_irqsave(&input_lock);
    char c;
    for (;;) {
        int scroll = take_scroll_request();
        if (scroll != 0) {
            spin_unlock_irqrestore(&input_lock, flags);
            screen_scroll_view(scroll);
            screen_flush();
            flags = spin_lock_irqsave(&input_lock);
            continue;
        }
        if (input_pop(&c)) {
            break;
        }
        if (task_killed()) {
            spin_unlock_irqrestore(&input_lock, flags);
            return -1;  // its process ended: give up so the task can die
        }
        wait_queue_sleep(&input_waiters, &input_lock);
    }
    spin_unlock_irqrestore(&input_lock, flags);
    return (unsigned char)c;
}

// Screen output with the Unix newline translation (ONLCR): the terminal treats
// LF as a pure line feed, as xterm does, so '\n' is sent as CR LF.
static void screen_out(char c) {
    if (c == '\n') {
        screen_putc('\r');
    }
    screen_putc(c);
}

void tty_putc(char c) {
#ifndef QEMU_TESTING
    if (screen_framebuffer() != NULL) {
        screen_out(c);
        return;
    }
#else
    screen_out(c);
#endif
    if (c == '\n') {
        uart_putc('\r');
    }
    uart_putc((unsigned char)c);
}
