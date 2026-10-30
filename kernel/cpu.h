/*
 * cpu.h - kleine CPU-Helfer als Inline-Funktionen (Port-I/O, MSRs, Interrupt-Flag)
 */
#ifndef BIYOS_CPU_H
#define BIYOS_CPU_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

/* Interrupts sperren und das alte Flag-Register zurueckgeben (kritische Abschnitte) */
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(f) : : "memory");
    return f;
}

/* Interrupts nur wieder einschalten, wenn sie vorher an waren (Verschachtelung ist erlaubt) */
static inline void irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile("sti" : : : "memory");
}

#endif /* BIYOS_CPU_H */
