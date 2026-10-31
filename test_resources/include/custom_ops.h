#ifndef CUSTOM_OPS_H
#define CUSTOM_OPS_H

#include <stdint.h>

static inline uint32_t padd8(uint32_t a, uint32_t b) {
    uint32_t result;
    asm volatile(
        ".insn r 0x0b, 0, 0, %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

static inline uint32_t psub8(uint32_t a, uint32_t b) {
    uint32_t result;
    asm volatile(
        ".insn r 0x0b, 0, 1, %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

static inline uint32_t pminu8(uint32_t a, uint32_t b) {
    uint32_t result;
    asm volatile(
        ".insn r 0x0b, 0, 2, %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

static inline uint32_t pmaxu8(uint32_t a, uint32_t b) {
    uint32_t result;
    asm volatile(
        ".insn r 0x0b, 0, 3, %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

#endif // CUSTOM_OPS_H
