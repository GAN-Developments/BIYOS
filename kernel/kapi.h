/* SPDX-License-Identifier: MIT */
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

#define KAPI_VERSION 5   /* 2: Tasks, Timer; 3: Tastatur; 4: Dateien; 5: Befehle, Programme, Module */

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

    /* --- Eingabe (ab Version 3) --- */
    int      (*getkey)(void);   /* naechstes Zeichen oder -1, wenn keins da ist */

    /* --- Dateien (ab Version 4): Pfade wie in der Shell, z.B. "E:\\dir\\datei" oder relativ --- */
    int      (*file_read_all)(const char *path, uint8_t **buf, uint64_t *len);   /* buf per malloc, 0-terminiert */
    int64_t  (*file_read)(const char *path, uint64_t off, void *buf, uint64_t len);
    int      (*file_write)(const char *path, const void *buf, uint64_t len, int append);
    int      (*file_mkdir)(const char *path);
    int      (*file_remove)(const char *path);
    int      (*file_rename)(const char *from, const char *to);

    /* --- Shell-Befehle, Programme und Module (ab Version 5) ---
     * cmd_register: fn(argc, argv) laeuft in der Shell-Task, argv[0] ist der Befehlsname. Ein angemeldeter
     *   Befehl hat Vorrang vor eingebauten (Befehle lassen sich also ersetzen). Wird das Modul, in dem fn
     *   liegt, entladen, verschwindet der Befehl von selbst.
     * exec_run:   Programm (PLSE/STRT) in der aufrufenden Task ausfuehren und warten; *ret (darf NULL sein)
     *   bekommt den Rueckgabewert. Rueckgabe 0 = gestartet, sonst Fehlercode.
     * exec_start: Programm in eigener Task starten, Rueckgabe Task-ID oder Fehlercode < 0.
     * module_load / module_unload: wie "module load" bzw. "module remove" in der Shell (DRIV). */
    int      (*cmd_register)(const char *name, int (*fn)(int argc, char **argv));
    int      (*cmd_unregister)(const char *name);
    int      (*exec_run)(const char *path, int argc, char **argv, int *ret);
    int64_t  (*exec_start)(const char *path, int argc, char **argv);
    int      (*module_load)(const char *path);
    int      (*module_unload)(const char *name_or_number);
} KernelApi;

extern KernelApi kapi;

#endif /* BIYOS_KAPI_H */
