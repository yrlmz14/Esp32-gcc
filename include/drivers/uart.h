#ifndef DRIVERS_UART_H
#define DRIVERS_UART_H

#include "esp32_regs.h"

struct UartRegs {
    volatile int fifo;       // 0x00: FIFO Data Read/Write
    volatile int int_raw;    // 0x04: Interrupt Raw
    volatile int int_st;     // 0x08: Interrupt Status
    volatile int int_ena;    // 0x0C: Interrupt Enable
    volatile int int_clr;    // 0x10: Interrupt Clear
    volatile int clkdiv;     // 0x14: Clock Divider
    volatile int autofid;    // 0x18: Auto Baud
    volatile int status;     // 0x1C: FIFO Status & Fill Counts
};

void uart_putc(char c);
void uart_puts(char *s);
void uart_put_int(int val);
void uart_put_hex(int val);

#endif // DRIVERS_UART_H
