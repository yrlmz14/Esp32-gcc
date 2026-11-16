// examples/hello_world.c
// Tiny Xtensa C Compiler: Native String Literal & Pointer Traversal targeting ESP32 UART0 MMIO

int hello_world() {
    volatile int *uart = (volatile int*)0x3FF40000;
    char *str = "Hello, World!\n";
    int count = 0;
    while (*str != 0) {
        *uart = *str;
        str = str + 1;
        count = count + 1;
    }
    return count;
}
