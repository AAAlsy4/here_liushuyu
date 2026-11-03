#include "io.h"
#include "syscall.h"
#include "trap.h"
#include <stdint.h>

/*
 * A small M-mode monitor.  The task frames intentionally use TrapContext:
 * trap.S can restore either the interrupted frame or a different task frame,
 * while mret performs the actual M -> U transition.
 */

#define MAX_TASKS 5
#define USER_STACK_SIZE 4096
#define TIMER_QUANTUM 64

#define SYS_PUTC 1
#define SYS_SLEEP 2
#define SYS_SPAWN 3
#define SYS_EXIT 4
#define SYS_YIELD 5

#define MSTATUS_MPIE (1ull << 7)
#define MSTATUS_MPP_MASK (3ull << 11)
#define MCAUSE_INTERRUPT (1ull << 63)

#define CLINT_MTIME 0x0200bff8ull
#define CLINT_MTIMECMP 0x02004000ull

typedef void (*task_entry_t)(void);

enum task_state {
    TASK_UNUSED,
    TASK_READY,
    TASK_SLEEPING,
    TASK_EXITED,
};

typedef struct {
    TrapContext context;
    uint8_t stack[USER_STACK_SIZE] __attribute__((aligned(16)));
    task_entry_t entry;
    enum task_state state;
    uint64_t wake_time;
    const char* name;
} Task;

static Task tasks[MAX_TASKS];
static TrapContext boot_context;
static unsigned current_task;
static unsigned task_count;
static unsigned exited_tasks;
static uint64_t expected_fault_pc;
static unsigned fault_checked;
static unsigned fault_passed;

static volatile uint64_t* const mtime = (volatile uint64_t*)CLINT_MTIME;
static volatile uint64_t* const mtimecmp = (volatile uint64_t*)CLINT_MTIMECMP;

static void copy_context(TrapContext* dst, const TrapContext* src) {
    for (unsigned i = 0; i < 32; ++i) {
        dst->x[i] = src->x[i];
    }
    dst->mstatus = src->mstatus;
    dst->mepc = src->mepc;
}

static long monitor_syscall(long nr, long a0, long a1) {
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x17 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(x11), "r"(x17) : "memory");
    return x10;
}

static long sys_putc(char ch) {
    return monitor_syscall(SYS_PUTC, (long)(unsigned char)ch, 0);
}

static long sys_sleep(uint64_t ticks) {
    return monitor_syscall(SYS_SLEEP, (long)ticks, 0);
}

static long sys_spawn(task_entry_t entry) {
    return monitor_syscall(SYS_SPAWN, (long)(uintptr_t)entry, 0);
}

static long sys_exit(long status) {
    return monitor_syscall(SYS_EXIT, status, 0);
}

static long sys_yield(void) {
    return monitor_syscall(SYS_YIELD, 0, 0);
}

static void user_puts(const char* text) {
    while (*text) {
        sys_putc(*text++);
    }
}

static void burn(uint64_t count) {
    volatile uint64_t value = 0;
    for (uint64_t i = 0; i < count; ++i) {
        value = value * 1664525u + i;
    }
    (void)value;
}

static void task_child(void);

static void task_round_robin(void) {
    user_puts("[U0] round-robin task started\n");
    for (unsigned i = 0; i < 6; ++i) {
        sys_putc('A' + (char)i);
        burn(500);
    }
    user_puts("\n[U0] exit\n");
    sys_exit(0);
}

static void task_sleeping(void) {
    user_puts("[U1] sleeping task started; spawning U3\n");
    sys_spawn(task_child);
    for (unsigned i = 0; i < 3; ++i) {
        user_puts("[U1] sleep\n");
        sys_sleep(180);
        sys_putc('S');
    }
    user_puts("\n[U1] exit\n");
    sys_exit(0);
}

static void task_fault(void) {
    user_puts("[U2] illegal-instruction test\n");
    /*
     * The monitor checks all three architectural trap values.  The label
     * makes mepc an exact, testable address rather than merely a nonzero PC.
     */
    expected_fault_pc = (uint64_t)(uintptr_t)&&illegal_instruction;
illegal_instruction:
    __asm__ volatile(".word 0xffffffff");
    user_puts("[U2] resumed after skipped illegal instruction\n");
    sys_exit(0);
}

static void task_child(void) {
    user_puts("[U3] spawned task running\n");
    for (unsigned i = 0; i < 4; ++i) {
        sys_putc('c');
        burn(350);
    }
    user_puts("\n[U3] exit\n");
    sys_exit(0);
}

static void task_init(Task* task, task_entry_t entry, const char* name) {
    task->entry = entry;
    task->state = TASK_READY;
    task->wake_time = 0;
    task->name = name;
    for (unsigned i = 0; i < 32; ++i) {
        task->context.x[i] = 0;
    }
    task->context.mstatus = 0;
    task->context.mepc = 0;
    task->context.x[2] = (uint64_t)(uintptr_t)(task->stack + USER_STACK_SIZE);
    task->context.mstatus = MSTATUS_MPIE & ~MSTATUS_MPP_MASK;
    task->context.mepc = (uint64_t)(uintptr_t)entry;
}

static int task_is_runnable(const Task* task, uint64_t now) {
    if (task->state == TASK_READY) {
        return 1;
    }
    if (task->state == TASK_SLEEPING && task->wake_time <= now) {
        return 1;
    }
    return 0;
}

static void wake_tasks(uint64_t now) {
    for (unsigned i = 0; i < task_count; ++i) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_time <= now) {
            tasks[i].state = TASK_READY;
        }
    }
}

static int all_tasks_exited(void) {
    return task_count != 0 && exited_tasks == task_count;
}

static int choose_next(uint64_t now) {
    wake_tasks(now);
    for (unsigned offset = 1; offset <= task_count; ++offset) {
        unsigned candidate = (current_task + offset) % task_count;
        if (task_is_runnable(&tasks[candidate], now)) {
            return (int)candidate;
        }
    }
    if (task_is_runnable(&tasks[current_task], now)) {
        return (int)current_task;
    }
    return -1;
}

static void arm_timer(void) {
    *mtimecmp = *mtime + TIMER_QUANTUM;
}

static void finish_monitor(int ok) {
    if (ok) {
        print_str("\n[M] monitor checks passed\n");
        pass();
    }
    print_str("\n[M] monitor checks failed\n");
    fail();
}

static void schedule_from_trap(TrapContext* trap_ctx, int next) {
    if (next < 0) {
        if (all_tasks_exited()) {
            finish_monitor(fault_checked && fault_passed);
        }
        arm_timer();
        return;
    }
    current_task = (unsigned)next;
    copy_context(trap_ctx, &tasks[current_task].context);
    arm_timer();
}

static void handle_ecall(TrapContext* trap_ctx) {
    Task* task = &tasks[current_task];
    long nr = (long)trap_ctx->x[17];
    long arg0 = (long)trap_ctx->x[10];
    int should_schedule = 0;

    trap_ctx->mepc += 4;
    switch (nr) {
    case SYS_PUTC:
        uart_putc((char)arg0);
        trap_ctx->x[10] = 0;
        break;
    case SYS_SLEEP:
        task->wake_time = *mtime + (uint64_t)arg0;
        task->state = TASK_SLEEPING;
        should_schedule = 1;
        trap_ctx->x[10] = 0;
        break;
    case SYS_SPAWN:
        if (task_count < MAX_TASKS && arg0 != 0) {
            task_init(&tasks[task_count], (task_entry_t)(uintptr_t)arg0, "spawned");
            ++task_count;
            trap_ctx->x[10] = (long)(task_count - 1);
        } else {
            trap_ctx->x[10] = -1;
        }
        break;
    case SYS_EXIT:
        task->state = TASK_EXITED;
        ++exited_tasks;
        should_schedule = 1;
        trap_ctx->x[10] = arg0;
        break;
    case SYS_YIELD:
        should_schedule = 1;
        trap_ctx->x[10] = 0;
        break;
    default:
        task->state = TASK_EXITED;
        ++exited_tasks;
        should_schedule = 1;
        trap_ctx->x[10] = -1;
        break;
    }

    copy_context(&tasks[current_task].context, trap_ctx);
    if (should_schedule) {
        schedule_from_trap(trap_ctx, choose_next(*mtime));
    }
}

void trap_handler(TrapContext* trap_ctx) {
    uint64_t cause = read_csr_mcause();
    uint64_t value = read_csr_mtval();

    if (cause == 8) {
        handle_ecall(trap_ctx);
        __traps_return(trap_ctx);
    }

    if (cause == (MCAUSE_INTERRUPT | 7)) {
        copy_context(&tasks[current_task].context, trap_ctx);
        tasks[current_task].state = TASK_READY;
        schedule_from_trap(trap_ctx, choose_next(*mtime));
        __traps_return(trap_ctx);
    }

    if (cause == 2) {
        ++fault_checked;
        if (trap_ctx->mepc == expected_fault_pc && value == 0xffffffffull) {
            ++fault_passed;
            print_str("[M] illegal instruction: mcause=2 mepc=");
            print_hex(trap_ctx->mepc, 0, '0');
            print_str(" mtval=0xffffffff\n");
        } else {
            print_str("[M] illegal instruction metadata mismatch mepc=");
            print_hex(trap_ctx->mepc, 0, '0');
            print_str(" expected=");
            print_hex(expected_fault_pc, 0, '0');
            print_str(" mtval=");
            print_hex(value, 0, '0');
            print_str("\n");
        }
        trap_ctx->mepc += 4;
        copy_context(&tasks[current_task].context, trap_ctx);
        __traps_return(trap_ctx);
    }

    print_str("[M] unexpected trap mcause=");
    print_hex(cause, 0, '0');
    print_str(" mtval=");
    print_hex(value, 0, '0');
    print_str(" mepc=");
    print_hex(trap_ctx->mepc, 0, '0');
    print_str(" task=");
    print_hex(current_task, 0, '0');
    print_str("\n");
    tasks[current_task].state = TASK_EXITED;
    ++exited_tasks;
    schedule_from_trap(trap_ctx, choose_next(*mtime));
    __traps_return(trap_ctx);
}

int main(void) {
    TEST_START("chapter 0x04 M-mode monitor");
    trap_init();

    task_init(&tasks[0], task_round_robin, "round-robin");
    task_init(&tasks[1], task_sleeping, "sleeping");
    task_init(&tasks[2], task_fault, "fault");
    task_count = 3;
    current_task = 0;
    arm_timer();

    copy_context(&boot_context, &tasks[0].context);
    __traps_return(&boot_context);
    return 0;
}
