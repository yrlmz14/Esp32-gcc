    .section .text
    .global _start
    .type _start, @function

_start:
    // Initialize Stack Pointer to top of SRAM (0x3FFE8000)
    movi a1, 0x3FFE8000

    // Call run_driver_demo() compiled by tiny-xtensa-cc
    call0 run_driver_demo

    // Return status is in a2 (expected: 1)
halt_loop:
    j halt_loop

    .size _start, . - _start
