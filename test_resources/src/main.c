#include "custom_ops.h"
#include "io.h"
#include "log.h"
#include "power.h"
#include <stdarg.h>
#include <stdint.h>

// 定义 UART 内存映射地址 (QEMU virt 平台)
#define UART_ADDR 0x10000000
#define UART_LSR_ADDR (UART_ADDR + 0x05)
#define UART_LSR_THRE 0x20  // Bit 5: Transmitter Holding Register Empty

extern char starttext[], endtext[], startrodata[], endrodata[], startdata[], enddata[],
    startbss[], endbss[], stack_lower_bound[], stack_top[];

void display_section_info() {
    Log(INFO, ".text section: [%08x, %08x]", (uintptr_t)(starttext), (uintptr_t)endtext);
    Log(INFO, ".rodata section: [%08x, %08x]", (uintptr_t)startrodata,
        (uintptr_t)endrodata);
    Log(INFO, ".data section: [%08x, %08x]", (uintptr_t)startdata, (uintptr_t)enddata);
    Log(INFO, ".bas section: [%08x, %08x]", (uintptr_t)startbss, (uintptr_t)endbss);
    Log(INFO, ".stack section: [%08x, %08x]", (uintptr_t)stack_lower_bound,
        (uintptr_t)stack_top);
}

// 程序入口点 (由链接脚本指定)
int main() {
    TEST_START(__BASE_FILE__);
    display_section_info();

    uint32_t a = 0x0A14FF01;
    uint32_t b = 0x05100A08;
    uint32_t add_res = padd8(a, b);
    uint32_t sub_res = psub8(a, b);
    uint32_t min_res = pminu8(a, b);
    uint32_t max_res = pmaxu8(a, b);
    printf("Custom packed-u8 finished!\n");
    printf("PADD8:  %x\n", add_res);
    printf("PSUB8:  %x\n", sub_res);
    printf("PMINU8: %x\n", min_res);
    printf("PMAXU8: %x\n", max_res);

    printf("Hello Qemu.\nformat test: %8d %08x %4o %c %s\n", 1ll, 255ll, 15, 'c', "test");
    Log(ERROR, "error test.");
    Log(WARN, "warnning test.");
    Log(DEBUG, "debug test.");
    Log(INFO, "info test.");
    Log(TRACE, "trace test.");

    pass();
    return 0;
}
