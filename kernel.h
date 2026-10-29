/*
 * kernel.h - gemeinsame Deklarationen des BIYOS-Kernels
 */
#ifndef BIYOS_KERNEL_H
#define BIYOS_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"

/* ---------- Konsole (kernel.c) ---------- */
void con_putc(char c);
void con_puts(const char *s);
void con_hex(uint64_t v);      /* ohne fuehrende Nullen */
void con_hex64(uint64_t v);    /* immer 16 Stellen */
void con_dec(uint64_t v);
void con_color(uint8_t r, uint8_t g, uint8_t b);
__attribute__((noreturn)) void kernel_halt(void);

/* ---------- Registerrahmen ----------
 * So liegt der Zustand auf dem Stack, wenn isr.S interrupt_handler() aufruft.
 * Die Reihenfolge entspricht den push-Befehlen in isr_common (niedrigste Adresse zuerst). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} Regs;

typedef void (*IrqHandler)(Regs *r);

/* ---------- CPU-Einrichtung (arch.c) ---------- */
void arch_init(const BootInfo *bi);                 /* GDT + TSS, IDT, Seitentabellen */
void idt_set_handler(uint8_t vector, IrqHandler fn); /* eigenen Handler fuer einen Vektor eintragen */

#endif /* BIYOS_KERNEL_H */
