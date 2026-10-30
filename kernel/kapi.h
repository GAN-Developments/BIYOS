/*
 * kapi.h - die Kernel-Funktionstabelle von BIYOS
 *
 * Alle austauschbaren Kernel-Dienste werden ueber diese Tabelle aufgerufen, auch vom Kernel
 * selbst. Weil es keinen Speicherschutz gibt, darf jedes Programm Eintraege ueberschreiben
 * (Dienst ersetzen oder erweitern, z.B. einen eigenen Seiten-Allokator oder eine eigene
 * Paging-Verwaltung). Den alten Zeiger vorher merken, wenn man ihn weiter nutzen will.
 *
 * Programme brauchen nur diese Datei. Der Loader uebergibt einen Zeiger auf die Tabelle.
 * 'size' und 'version' erlauben spaeter neue Felder am Ende, ohne alte Programme zu brechen.
 */
#ifndef BIYOS_KAPI_H
#define BIYOS_KAPI_H

#include <stdint.h>
#include <stddef.h>

#define KAPI_VERSION 2   /* 2: Tasks, Timer */

/* Registerrahmen, wie ihn isr.S beim Interrupt auf dem Stack aufbaut */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} Regs;

typedef void (*IrqHandler)(Regs *r);

/* Seitentabellen-Flags (rohe x86_64-PTE-Bits, werden unveraendert uebernommen) */
#define PG_PRESENT 0x001ULL
#define PG_WRITE   0x002ULL
#define PG_WC      0x008ULL   /* PWT: Write-Combining (PAT-Index 1) */
#define PG_UC      0x018ULL   /* PWT|PCD: Uncacheable (PAT-Index 3) */
#define PG_NX      (1ULL << 63)

typedef struct KernelApi {
    uint32_t size;      /* sizeof(KernelApi) */
    uint32_t version;   /* KAPI_VERSION */

    /* --- Konsole --- */
    void (*putc)(char c);
    void (*puts)(const char *s);
    void (*hex)(uint64_t v);
    void (*dec)(uint64_t v);
    void (*color)(uint8_t r, uint8_t g, uint8_t b);

    /* --- Physischer Speicher (4-KiB-Seiten, zusammenhaengend) --- */
    uint64_t (*page_alloc)(uint64_t count);               /* physische Adresse oder 0 */
    void     (*page_free)(uint64_t addr, uint64_t count);

    /* --- Heap --- */
    void *(*malloc)(size_t n);
    void  (*free)(void *p);
    void *(*realloc)(void *p, size_t n);

    /* --- Paging (arbeitet auf der aktuellen CR3-Wurzel) --- */
    int       (*map)(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);   /* 0 = ok */
    int       (*unmap)(uint64_t virt, uint64_t size);
    uint64_t *(*root)(void);                               /* aktuelle PML4 */

    /* --- Interrupts --- */
    void (*set_handler)(uint8_t vector, IrqHandler fn);

    /* --- Tasks und Zeit (ab Version 2) --- */
    uint64_t (*task_create)(const char *name, void (*entry)(void *), void *arg);   /* Task-ID oder 0 */
    int      (*task_kill)(uint64_t id);                                             /* 0 = ok */
    void     (*task_exit)(void);                                                    /* beendet die aufrufende Task */
    void     (*yield)(void);                                                        /* CPU freiwillig abgeben */
    void     (*sleep_ms)(uint64_t ms);
    uint64_t (*uptime_ms)(void);
} KernelApi;

extern KernelApi kapi;

#endif /* BIYOS_KAPI_H */
