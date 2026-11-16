#include <stdint.h>

extern int test_math(void);
extern int sum_to_n(int n);
extern int fib(int n);
extern int test_array_sum(void);
extern int test_sort(void);
extern int test_macro(void);
extern int test_struct_dot(void);
extern int test_struct_arrow(void);
extern int test_uart_struct(void);
extern int test_for_sum(void);
extern int test_break_continue(void);
extern int test_nested_break(void);
extern int test_ternary(int a, int b);
extern int test_logical(int a, int b);
extern int test_compound(void);
extern int test_inc_dec(void);
extern int test_do_while(void);
extern int test_switch(int mode);
extern int test_c99_typedef(void);
extern int test_c99_enum(int s);
extern int test_c99_bool(int val);
extern int test_c99_inits(void);
extern int test_c99_stdint(void);
extern int test_cpp_class(void);
extern int test_cpp_ref(void);
extern int test_cpp_ctor(void);
extern int test_cpp_ns(void);
extern int test_gcc_attr(void);
extern int test_stmt_expr(void);
extern int test_bitfield(void);
extern int test_union(void);
extern int test_designated(void);
extern int test_fn_ptr(void);

// Global results array in DRAM
volatile int g_results[36];
volatile int g_done = 0;

void runner_main(void) {
    // Execute compiled functions
    g_results[0] = test_math();          // Expected: 65
    g_results[1] = sum_to_n(10);         // Expected: 55
    g_results[2] = fib(7);               // Expected: 13
    g_results[3] = fib(0);               // Expected: 0
    g_results[4] = fib(1);               // Expected: 1
    g_results[5] = sum_to_n(1);          // Expected: 1
    g_results[6] = test_array_sum();     // Expected: 150
    g_results[7] = test_sort();          // Expected: 6708
    g_results[8] = test_macro();         // Expected: 50
    g_results[9] = test_struct_dot();    // Expected: 40
    g_results[10] = test_struct_arrow(); // Expected: 300
    g_results[11] = test_uart_struct();  // Expected: 8
    g_results[12] = test_for_sum();      // Expected: 55
    g_results[13] = test_break_continue();// Expected: 40
    g_results[14] = test_nested_break(); // Expected: 12
    g_results[15] = test_ternary(7, 3);  // Expected: 71
    g_results[16] = test_logical(5, 10); // Expected: 1
    g_results[17] = test_compound();     // Expected: 24
    g_results[18] = test_inc_dec();      // Expected: 10
    g_results[19] = test_do_while();     // Expected: 55
    g_results[20] = test_switch(2);      // Expected: 200
    g_results[21] = test_c99_typedef();  // Expected: 142
    g_results[22] = test_c99_enum(10);   // Expected: 2
    g_results[23] = test_c99_bool(5);    // Expected: 100
    g_results[24] = test_c99_inits();    // Expected: 400
    g_results[25] = test_c99_stdint();   // Expected: 1200
    g_results[26] = test_cpp_class();    // Expected: 42
    g_results[27] = test_cpp_ref();      // Expected: 88
    g_results[28] = test_cpp_ctor();     // Expected: 40
    g_results[29] = test_cpp_ns();       // Expected: 25
    g_results[30] = test_gcc_attr();     // Expected: 142
    g_results[31] = test_stmt_expr();    // Expected: 205
    g_results[32] = test_bitfield();     // Expected: 1019
    g_results[33] = test_union();        // Expected: 120
    g_results[34] = test_designated();   // Expected: 350
    g_results[35] = test_fn_ptr();       // Expected: 52

    g_done = 1;
}
