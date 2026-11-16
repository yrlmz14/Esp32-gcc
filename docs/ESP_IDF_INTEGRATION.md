# Integrating `tiny-xtensa-cc` with ESP-IDF & FreeRTOS

This guide explains how to compile real ESP-IDF projects and custom components with `tiny-xtensa-cc`.

---

## 1. ESP-IDF Architectural Overview

In a typical ESP-IDF project:
- **Build System**: CMake drives compilation via `idf.py build`.
- **Target CPU**: Xtensa LX6 (ESP32) / LX7 (ESP32-S2 / ESP32-S3).
- **Toolchain**: `xtensa-esp32-elf-gcc`, `xtensa-esp32-elf-ld`, `xtensa-esp32-elf-gdb`.
- **Operating System**: FreeRTOS (ESP-IDF port).

Because `tiny-xtensa-cc` produces standard **ELF32 relocatable object files (`.o`)** matching the **Xtensa CALL0 ABI**, it can compile individual components, peripheral drivers, or entire application modules that link into the final firmware binary.

---

## 2. Using `tiny-xtensa-cc` with CMake / ESP-IDF Components

In an ESP-IDF project, you can compile specific source files or components using `tiny-xtensa-cc` in your `CMakeLists.txt`:

```cmake
# In your component's CMakeLists.txt:

set(TINY_CC "${CMAKE_SOURCE_DIR}/tools/tiny-xtensa-cc")

# Define target custom command for compiling with tiny-xtensa-cc
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/my_driver.o
    COMMAND ${TINY_CC} -I${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/my_driver.c -o ${CMAKE_CURRENT_BINARY_DIR}/my_driver.o
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/my_driver.c
    COMMENT "Compiling my_driver.c using tiny-xtensa-cc"
)

# Register the object file into the component library
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "include"
)

target_link_libraries(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/my_driver.o)
```

---

## 3. Writing ESP-IDF Drivers for `tiny-xtensa-cc`

### 1. Hardware Register Access with MMIO Structs
Use volatile struct layouts rather than hardcoded magic offsets:

```c
#include <stdint.h>
#include <drivers/esp32_regs.h>
#include <drivers/gpio.h>

void blink_led(int pin) {
    volatile struct GpioDev *gpio = (volatile struct GpioDev *)GPIO_BASE;

    // Enable pin output
    gpio->enable |= (1 << pin);
    __asm__ volatile ("memw");

    // Set pin HIGH
    gpio->out_w1ts = (1 << pin);
    __asm__ volatile ("memw");

    // Delay loop
    for (volatile int i = 0; i < 100000; i++) { }

    // Set pin LOW
    gpio->out_w1tc = (1 << pin);
    __asm__ volatile ("memw");
}
```

### 2. FreeRTOS Tasks with Function Pointers
```c
typedef void (*TaskFunction_t)(void *pvParameters);

void vTaskCode(void *pvParameters) {
    while (1) {
        // Task work
    }
}

void setup_task(void) {
    TaskFunction_t task_fn = vTaskCode;
    // Pass task_fn to xTaskCreate or scheduler
}
```

### 3. ESP-IDF Error Checking Macros
```c
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;

#define ESP_ERROR_CHECK(x) ({ esp_err_t err = (x); if (err != ESP_OK) return err; err; })

esp_err_t init_hardware(void) {
    ESP_ERROR_CHECK(ESP_OK);
    return ESP_OK;
}
```

---

## 4. Verification in Headless QEMU

You can simulate any compiled ELF on the official Espressif QEMU simulator:

```bash
# Launch QEMU with GDB server attached
qemu-system-xtensa -nographic -machine esp32 -drive file=/dev/null,if=mtd,format=raw -s -S -kernel build/app.elf &

# Connect GDB to run and inspect execution
xtensa-esp32-elf-gdb -batch -ex "target remote :1234" -ex "set \$pc = _start" -ex "continue" -ex "quit" build/app.elf
```
