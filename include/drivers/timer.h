#ifndef DRIVERS_TIMER_H
#define DRIVERS_TIMER_H

#include "esp32_regs.h"

struct TimerDev {
    volatile int config;     // 0x00: Timer Config
    volatile int cnt_low;    // 0x04: Counter Low
    volatile int cnt_high;   // 0x08: Counter High
    volatile int update;     // 0x0C: Counter Update
    volatile int alarm_low;  // 0x10: Alarm Low
    volatile int alarm_high; // 0x14: Alarm High
    volatile int load_low;   // 0x18: Reload Low
    volatile int load_high;  // 0x1C: Reload High
    volatile int reload;     // 0x20: Reload Trigger
};

void timer_init(void);
void delay_cycles(int count);

#endif // DRIVERS_TIMER_H
