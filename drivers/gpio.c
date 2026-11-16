#include "include/drivers/gpio.h"

#define GPIO_SHADOW_ADDR 0x3FFE1000

void gpio_set_direction(int pin, int mode) {
    struct GpioDev *gpio = (struct GpioDev*)GPIO_BASE;
    int mask = 1 << pin;
    if (mode != 0) {
        gpio->enable_w1ts = mask;
    } else {
        gpio->enable_w1tc = mask;
    }
}

void gpio_set_level(int pin, int level) {
    struct GpioDev *gpio = (struct GpioDev*)GPIO_BASE;
    volatile int *shadow = (volatile int*)GPIO_SHADOW_ADDR;
    int mask = 1 << pin;
    int cur = *shadow;
    if (level != 0) {
        gpio->out_w1ts = mask;
        *shadow = cur | mask;
    } else {
        gpio->out_w1tc = mask;
        if ((cur & mask) != 0) {
            *shadow = cur - mask;
        }
    }
}

int gpio_get_level(int pin) {
    struct GpioDev *gpio = (struct GpioDev*)GPIO_BASE;
    volatile int *shadow = (volatile int*)GPIO_SHADOW_ADDR;
    int val = *shadow;
    return (val >> pin) & 1;
}

void gpio_toggle(int pin) {
    int current = gpio_get_level(pin);
    if (current != 0) {
        gpio_set_level(pin, 0);
    } else {
        gpio_set_level(pin, 1);
    }
}
