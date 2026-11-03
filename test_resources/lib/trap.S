.altmacro
.macro SAVE_REG n
    sd x\n, \n*8(sp)
.endm

.macro LOAD_REG n
    ld x\n, \n*8(sp)
.endm

.globl __traps_entry
.globl __traps_return
.align 2
__traps_entry:
    csrrw sp, mscratch, sp
    # now sp->kernel stack, mscratch->user stack
    # Allocate the frame on the dedicated M-mode trap stack.  The handler may
    # return a different TrapContext, so the original frame address must not
    # be derived from that replacement context.
    addi sp, sp, -34*8
    # save general-purpose registers
    sd x1, 1*8(sp)
    # skip sp(x2), we will save it later
    sd x3, 3*8(sp)
    # skip tp(x4), application does not use it
    # Save t0~t2 before using them as trap-entry temporaries.
    sd x5, 5*8(sp)
    sd x6, 6*8(sp)
    sd x7, 7*8(sp)
    la t0, __trap_frame_pointer
    sd sp, 0(t0)
    # save x8~x31
    .set n, 8
    .rept 24
        SAVE_REG %n
        .set n, n+1
    .endr
    # t0/t1/t2 are now saved and may be used freely.
    csrr t0, mstatus
    csrr t1, mepc
    sd t0, 32*8(sp)
    sd t1, 33*8(sp)
    # read user stack from mscratch and save it on the kernel stack
    csrr t2, mscratch
    sd t2, 2*8(sp)
    # set input argument of trap_handler(cx: &mut TrapContext)
    mv a0, sp
    call trap_handler


__traps_return:
    # case1: start running app by __restore
    # case2: back to U after handling trap
    mv sp, a0
    # now sp->kernel stack(after allocated), mscratch->user stack
    # restore mstatus/mepc
    ld t0, 32*8(sp)
    ld t1, 33*8(sp)
    ld t2, 2*8(sp)
    csrw mstatus, t0
    csrw mepc, t1
    csrw mscratch, t2
    # restore general-purpuse registers except sp/tp
    ld x1, 1*8(sp)
    ld x3, 3*8(sp)
    .set n, 5
    .rept 27
        LOAD_REG %n
        .set n, n+1
    .endr
    # Restore the original M-mode stack, even when a different task context
    # was selected.  tp is intentionally not part of TrapContext.
    la tp, __trap_frame_pointer
    ld tp, 0(tp)
    addi tp, tp, 34*8
    csrrw sp, mscratch, tp
    mret

.section .bss
.align 3
.globl __trap_frame_pointer
__trap_frame_pointer:
    .dword 0
