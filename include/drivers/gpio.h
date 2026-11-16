#ifndef DRIVERS_GPIO_H
#define DRIVERS_GPIO_H

#include "esp32_regs.h"

struct GpioDev {
    volatile int out;        // 0x00: GPIO_OUT_REG
    volatile int out_w1ts;   // 0x04: GPIO_OUT_W1TS_REG (Set)
    volatile int out_w1tc;   // 0x08: GPIO_OUT_W1TC_REG (Clear)
    volatile int enable;     // 0x0C: GPIO_ENABLE_REG
    volatile int enable_w1ts;// 0x10: GPIO_ENABLE_W1TS_REG
    volatile int enable_w1tc;// 0x14: GPIO_ENABLE_W1TC_REG
    volatile int pad0;       // 0x18
    volatile int pad1;       // 0x1C
    volatile int pad2;       // 0x20
    volatile int pad3;       // 0x24
    volatile int pad4;       // 0x28
    volatile int pad5;       // 0x2C
    volatile int pad6;       // 0x30
    volatile int pad7;       // 0x34
    volatile int pad8;       // 0x38
    volatile int in;         // 0x3C: GPIO_IN_REG
};

void gpio_set_direction(int pin, int mode);
void gpio_set_level(int pin, int level);
int  gpio_get_level(int pin);
void gpio_toggle(int pin);

#endif // DRIVERS_GPIO_H
