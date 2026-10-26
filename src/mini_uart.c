#include "mini_uart.h"
#include "lib/panic.h"
#include "gpio.h"
#include "peripherals/irq.h"
#include "utils.h"

#if defined(QEMU_TESTING) || defined(KORAOS_VIRT)
// Use PL011 UART for QEMU which has better support
#include "peripherals/pl011.h"

void uart_init(void) {
    // Disable UART
    REGS_PL011->cr = 0;
    
    // Wait for end of transmission
    while (REGS_PL011->fr & (1 << 3)) { }
    
#ifndef KORAOS_VIRT
    // Configure GPIO pins 14 and 15 for PL011 UART (Alt0)
    uint32_t selector = REGS_GPIO->func_select[1];
    selector &= ~((7 << 12) | (7 << 15));  // Clear pins 14 and 15
    selector |= (4 << 12) | (4 << 15);     // Set to Alt0 (PL011 UART)
    REGS_GPIO->func_select[1] = selector;
    
    // TX is driven by the UART; pull RX up so a line with nothing driving it
    // (adapter unplugged, loose wire) idles high instead of floating, which
    // the receiver would decode as a stream of noise bytes.
    gpio_pin_set_pull(14, GPNone);
    gpio_pin_set_pull(15, GPUp);
    
#endif

    // Clear pending interrupts
    REGS_PL011->icr = 0x7FF;
    
    // Set baud rate to 115200
    // UART clock = 3MHz, baud = 115200
    // Divisor = 3000000 / (16 * 115200) = 1.627
    // Integer part = 1, Fractional part = 0.627 * 64 = 40
#ifdef KORAOS_VIRT
    // QEMU virt exposes a fixed 24 MHz PL011 clock.
    REGS_PL011->ibrd = 13;
    REGS_PL011->fbrd = 1;
#else
    REGS_PL011->ibrd = 1;
    REGS_PL011->fbrd = 40;
#endif
    
    // Enable FIFO, 8-bit data, 1 stop bit, no parity
    REGS_PL011->lcrh = (1 << 4) | (3 << 5);
    
    // Mask all interrupts
    REGS_PL011->imsc = 0;
    
    // Enable UART, transmit, and receive
    REGS_PL011->cr = (1 << 0) | (1 << 8) | (1 << 9);
}

void uart_putc(unsigned char c) {
    // Wait until transmit FIFO is not full (FR bit 5)
    while (REGS_PL011->fr & (1 << 5)) {
        // Wait
    }
    
    // Write the character
    REGS_PL011->dr = (uint32_t)c;
}

unsigned char uart_getc(void) {
    // Wait until receive FIFO is not empty (FR bit 4)
    while (REGS_PL011->fr & (1 << 4)) {
        // Wait
    }
    
    // Read and return the character
    return (unsigned char)(REGS_PL011->dr & 0xFF);
}

int uart_rx_ready(void) {
    return !(REGS_PL011->fr & (1 << 4));  // RX FIFO not empty
}

void uart_rx_irq_enable(irq_handler_t handler) {
    irq_connect(IRQ_UART0, handler, NULL);
    REGS_PL011->imsc = (1 << 4) | (1 << 6);  // RX + RX timeout (FIFO enabled)
}

#else
// Use Mini UART for real hardware
#include "peripherals/aux.h"

void uart_init(void) {
    // Enable mini UART (this also enables access to its registers)
    REGS_AUX->enables = 1;
    
    // Disable transmitter and receiver during configuration
    REGS_AUX->mu_control = 0;
    
    // Disable interrupts
    REGS_AUX->mu_ier = 0;
    
    // Clear FIFOs
    REGS_AUX->mu_iir = 0xC6;
    
    // Set 8-bit mode
    REGS_AUX->mu_lcr = 0x03;
    
    // Set baud rate to 115200
    // Baud rate = system_clock / (8 * (baudrate_reg + 1))
    // For 250MHz system clock: 250000000 / (8 * 115200) = 270
#if RPI_VERSION == 4
    REGS_AUX->mu_baud_rate = 541;   // 500 MHz core / 115200 baud
#elif RPI_VERSION == 3
    REGS_AUX->mu_baud_rate = 270;   // 250 MHz core / 115200 baud
#else
    REGS_AUX->mu_baud_rate = 270;
#endif    
    // Configure GPIO pins 14 and 15 for Mini UART (Alt5)
    uint32_t selector = REGS_GPIO->func_select[1];
    selector &= ~((7 << 12) | (7 << 15));  // Clear pins 14 and 15
    selector |= (2 << 12) | (2 << 15);     // Set to Alt5 (Mini UART)
    REGS_GPIO->func_select[1] = selector;
    
    // TX is driven by the UART; pull RX up so a line with nothing driving it
    // (adapter unplugged, loose wire) idles high instead of floating, which
    // the receiver would decode as a stream of noise bytes.
    gpio_pin_set_pull(14, GPNone);
    gpio_pin_set_pull(15, GPUp);
    
    // Enable transmitter and receiver
    REGS_AUX->mu_control = 0x03;
}

void uart_putc(unsigned char c) {
    // Wait until transmitter is ready (LSR bit 5 = transmitter empty)
    while (!(REGS_AUX->mu_lsr & (1 << 5))) {
        // Wait
    }
    
    // Write the character
    REGS_AUX->mu_io = (uint32_t)c;
}

unsigned char uart_getc(void) {
    // Wait until data is available (LSR bit 0 = data ready)
    while (!(REGS_AUX->mu_lsr & (1 << 0))) {
        // Wait
    }
    
    // Read and return the character
    return (unsigned char)(REGS_AUX->mu_io & 0xFF);
}

int uart_rx_ready(void) {
    return REGS_AUX->mu_lsr & (1 << 0);  // LSR data ready
}

void uart_rx_irq_enable(irq_handler_t handler) {
    irq_connect(IRQ_AUX, handler, NULL);
    // Receive interrupt only. Per the BCM2835 datasheet errata, bit 0 (not 1)
    // enables RX, and bits 3:2 must be set for interrupts to be raised at all.
    REGS_AUX->mu_ier = 0x0D;
}
#endif

// Hardware-agnostic helper function: the whole string in one piece, under
// the serial output lock (other cores' lines never land in the middle).
void uart_puts(const char *str) {
    unsigned long flags = serial_out_begin();
    while (*str) {
        if (*str == '\n') {
            uart_putc('\r');
        }
        uart_putc(*str++);
    }
    serial_out_end(flags);
}
