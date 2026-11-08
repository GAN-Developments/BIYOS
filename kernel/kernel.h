/* SPDX-License-Identifier: MIT */
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
#include "blk.h"
#include "fs.h"

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

/* ---------- Konsole, Erweiterungen (kernel.c) ---------- */
extern const BootInfo *g_bi;
void con_reset(void);       /* Bildschirm leeren */
void con_backspace(void);   /* ein Zeichen zurueck und loeschen */

/* ---------- ACPI / IOAPIC (acpi.c) ---------- */
int acpi_init(uint64_t rsdp);
int ioapic_route_irq(uint8_t isa_irq, uint8_t vector);

/* ---------- Local APIC und Scheduler-Zusatz (sched.c) ---------- */
uint32_t lapic_id(void);
void     lapic_eoi(void);
void     sched_request(void);   /* am Ende des Interrupts einen Taskwechsel anfordern */
void     sched_list(void);

/* ---------- Tastatur (kbd.c) ---------- */
void kbd_init(void);
int  kbd_getc(void);            /* Zeichen oder -1 */
void kbd_tick(void);            /* Polling-Modus: vom Timer aufgerufen */
void kbd_set_layout(int de);
int  kbd_layout_de(void);

/* ---------- PCI (pci.c) ---------- */
typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  cls, sub, prog;
} PciDev;

void           pci_init(void);
int            pci_count(void);
const PciDev  *pci_get(int i);
const PciDev  *pci_find_class(uint8_t cls, uint8_t sub, uint8_t prog, int index);   /* prog 0xFF = egal */
uint32_t       pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void           pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
void           pci_enable(const PciDev *d);
uint64_t       pci_bar(const PciDev *d, int n, uint64_t *size);
const char    *pci_class_name(uint8_t cls, uint8_t sub, uint8_t prog);

/* ---------- Datentraeger (nvme.c, blk.c) ---------- */
int        nvme_init(void);
int        blk_register(BlockDev *d);
int        blk_count(void);
BlockDev  *blk_get(int i);
BlockDev  *blk_find(const char *name);
void       blk_scan_partitions(BlockDev *disk);
void       storage_init(void);   /* PCI scannen, Laufwerke und Partitionen einrichten */

/* ---------- Dateisysteme (fat32.c, vfs.c) ---------- */
int   fat32_mount(BlockDev *bd, Fs **out);          /* schreibgeschuetzt eingebunden zurueckgegeben */
int   ntfs_mount(BlockDev *bd, Fs **out);           /* dito (NTFS bisher nur lesen) */
int   vfs_mount(char letter, Fs *fs);
int   vfs_unmount(char letter);
Fs   *vfs_get(char letter);
int   vfs_mount_dev(const char *devname, char letter, int rw);
int   vfs_set_rw(char letter, int rw);
void  vfs_automount(void);
char  vfs_cur_drive(void);
const char *vfs_cur_path(void);
int   vfs_set_drive(char letter);
int   vfs_cd(const char *path);
int   vfs_stat(const char *path, FsEntry *e);
int   vfs_readdir(const char *path, FsDirCb cb, void *ctx);
int64_t vfs_read(const char *path, uint64_t off, void *buf, uint64_t len);
int   vfs_read_all(const char *path, uint8_t **buf, uint64_t *len);   /* malloc'd, 0-terminiert */
int   vfs_write(const char *path, const void *buf, uint64_t len, int append);
int   vfs_mkdir(const char *path);
int   vfs_remove(const char *path);
int   vfs_rename(const char *from, const char *to);
int   vfs_statfs(char letter, uint64_t *total, uint64_t *free_bytes);

/* ---------- Executables und Module (exec.c) ---------- */
#define EX_EHEADER -20   /* kein gueltiger BIYOS-Header */
#define EX_ETYPE   -21   /* falscher Typ (DRIV/STRT/PLSE) fuer diesen Befehl */
#define EX_EIMPORT -22   /* IMPORT-Abschnitt ungueltig oder Quelle nicht lesbar */
#define EX_ENOCODE -23   /* nach dem Header kommt kein Code */
#define EX_EINIT   -24   /* Modul meldet beim Start einen Fehler */
#define EX_EBUSY   -25   /* Modul lehnt das Entladen ab */
#define EX_ELIMIT  -26   /* Modultabelle voll */
#define EX_ENOMOD  -27   /* Modul nicht geladen */

const char *exec_strerror(int err);                                       /* auch fuer FS_E*-Codes */
int     exec_run(const char *path, int argc, char **argv, int *ret);      /* in der aufrufenden Task, wartet */
int64_t exec_start(const char *path, int argc, char **argv);              /* eigene Task: ID oder Fehler < 0 */
int     module_load(const char *path);
int     module_unload(const char *name_or_number);
void    module_list(void);
void    module_load_boot(const BootInfo *bi);

/* ---------- Shell (shell.c) ---------- */
void shell_main(void *arg);
int  shell_cmd_register(const char *name, int (*fn)(int argc, char **argv));
int  shell_cmd_unregister(const char *name);
void shell_cmds_remove_range(uint64_t lo, uint64_t hi);   /* alle Befehle, deren Code in [lo, hi) liegt */

#endif /* BIYOS_KERNEL_H */
