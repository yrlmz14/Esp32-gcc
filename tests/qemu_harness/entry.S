.section .text
.align 4
.global _start
.type _start, @function
_start:
    # Initialize stack pointer in ESP32 internal DRAM (0x3FFE8000)
    movi a1, 0x3FFE8000

    # Call main test runner
    call0 runner_main

    # Halt loop
halt_loop:
    j halt_loop
