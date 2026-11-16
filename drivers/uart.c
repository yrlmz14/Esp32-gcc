#include "include/drivers/uart.h"

void uart_putc(char c) {
    struct UartRegs *uart = (struct UartRegs*)UART0_BASE;
    uart->fifo = (int)c;
}

void uart_puts(char *s) {
    while (*s != 0) {
        uart_putc(*s);
        s = s + 1;
    }
}

void uart_put_int(int val) {
    if (val == 0) {
        uart_putc(48); // '0'
        return;
    }
    if (val < 0) {
        uart_putc(45); // '-'
        val = -val;
    }
    char buf[16];
    int idx = 0;
    while (val > 0) {
        int rem = val % 10;
        buf[idx] = (char)(48 + rem);
        idx = idx + 1;
        val = val / 10;
    }
    while (idx > 0) {
        idx = idx - 1;
        uart_putc(buf[idx]);
    }
}

void uart_put_hex(int val) {
    uart_puts("0x");
    char hex_digits[16];
    hex_digits[0] = 48; hex_digits[1] = 49; hex_digits[2] = 50; hex_digits[3] = 51;
    hex_digits[4] = 52; hex_digits[5] = 53; hex_digits[6] = 54; hex_digits[7] = 55;
    hex_digits[8] = 56; hex_digits[9] = 57; hex_digits[10] = 65; hex_digits[11] = 66;
    hex_digits[12] = 67; hex_digits[13] = 68; hex_digits[14] = 69; hex_digits[15] = 70;

    int i = 7;
    while (i >= 0) {
        int nibble = (val >> (i * 4)) & 15;
        uart_putc(hex_digits[nibble]);
        i = i - 1;
    }
}
