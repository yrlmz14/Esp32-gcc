#include "include/drivers/timer.h"

void timer_init(void) {
    struct TimerDev *timg = (struct TimerDev*)TIMG0_BASE;
    timg->config = 0;
}

void delay_cycles(int count) {
    int i = 0;
    while (i < count) {
        i = i + 1;
    }
}
