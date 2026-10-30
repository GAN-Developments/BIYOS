/*
 * kernel.h - gemeinsame Deklarationen des BIYOS-Kernels
 */
#ifndef BIYOS_KERNEL_H
#define BIYOS_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"
#include "kapi.h"
#include "cpu.h"

/* ---------- Konsole (kernel.c) ---------- */
void con_putc(char c);
void con_puts(const char *s);
void con_hex(uint64_t v);      /* ohne fuehrende Nullen */
void con_hex64(uint64_t v);    /* immer 16 Stellen */
void con_dec(uint64_t v);
void con_color(uint8_t r, uint8_t g, uint8_t b);
__attribute__((noreturn)) void kernel_halt(void);

/* ---------- CPU-Einrichtung (arch.c) ---------- */
void arch_init(const BootInfo *bi);                 /* GDT + TSS, IDT, Seitentabellen */
void idt_set_handler(uint8_t vector, IrqHandler fn); /* eigenen Handler fuer einen Vektor eintragen */

/* ---------- Speicherverwaltung (mm.c) ----------
 * Diese Funktionen sind die Standardimplementierungen, die in der Tabelle 'kapi' stehen.
 * Der Kernel ruft sie ueber kapi auf, damit sie austauschbar sind. */
void     mm_init(const BootInfo *bi);
uint64_t pmm_alloc_pages(uint64_t count);
void     pmm_free_pages(uint64_t addr, uint64_t count);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_page_count(void);
void    *heap_malloc(size_t n);
void     heap_free(void *p);
void    *heap_realloc(void *p, size_t n);
int      paging_map(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);
int      paging_unmap(uint64_t virt, uint64_t size);
uint64_t *paging_root(void);

/* ---------- Timer und Scheduler (sched.c) ---------- */
void     sched_init(void);   /* Local-APIC-Timer, Scheduler starten, Interrupts einschalten */
Regs    *sched_after_interrupt(Regs *r);   /* von arch.c am Ende jedes Interrupts aufgerufen */
uint64_t task_create(const char *name, void (*entry)(void *), void *arg);
int      task_kill(uint64_t id);
void     task_exit(void);
void     sched_yield(void);
void     sched_sleep_ms(uint64_t ms);
uint64_t timer_uptime_ms(void);

#endif /* BIYOS_KERNEL_H */
