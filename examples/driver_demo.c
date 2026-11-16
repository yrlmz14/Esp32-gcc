#include "include/drivers/esp32_regs.h"
#include "include/drivers/gpio.h"
#include "include/drivers/uart.h"
#include "include/drivers/timer.h"

int run_driver_demo() {
    // 1. Initialize GPIO 2 (LED Pin) as output
    gpio_set_direction(2, 1);
    gpio_set_level(2, 1);

    // 2. Output formatted strings and numbers via UART
    uart_puts("ESP32 Driver Demo Running!\n");
    uart_puts("GPIO 2 Status: ");
    uart_put_int(gpio_get_level(2));
    uart_puts("\nHex Value: ");
    uart_put_hex(42);
    uart_puts("\n");

    // 3. Toggle GPIO
    gpio_toggle(2);
    uart_puts("GPIO 2 After Toggle: ");
    uart_put_int(gpio_get_level(2));
    uart_puts("\n");

    // 4. Delay
    delay_cycles(100);

    return 1;
}
