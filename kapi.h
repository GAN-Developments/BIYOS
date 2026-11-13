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
#include "blk.h"   /* BlockDev: fuer blk_register */
#include "fs.h"    /* FsEntry, FsDirCb und die FS_E*-Fehlercodes */

#define KAPI_VERSION 10   /* 2: Tasks, Timer; 3: Tastatur; 4: Dateien; 5: Befehle, Programme, Module; 6: Hardware fuer Treiber; 7: Shell ersetzen; 8: Laufwerke, Zellkonsole, Editor; 9: Scancodes einspeisen (USB-Tastatur); 10: Sprache */

/* Sondertasten, wie kbd_inject sie annimmt und getkey sie liefert (Zeichen ab 0x80; Strg+Buchstabe = 1..26) */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_HOME  0x84
#define KEY_END   0x85
#define KEY_PGUP  0x86
#define KEY_PGDN  0x87
#define KEY_DEL   0x88
#define KEY_INS   0x89

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

/* PCI-Gerät, wie pci_get / pci_find_class es liefern */
typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  cls, sub, prog;
} PciDev;

/* Infos zu einem eingebundenen Laufwerk (drive_info) */
typedef struct {
    char    type[8];     /* "FAT32", "NTFS" */
    char    label[16];
    char    dev[16];     /* Blockgeraet, z.B. "nvme0p1" */
    uint8_t readonly;    /* 1 = schreibgeschuetzt (mit file_set_rw aenderbar) */
} DriveInfo;

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

    /* --- Hardware fuer Treiber (ab Version 6) ---
     * PCI: pci_find_class liefert das index-te Geraet einer Klasse (prog 0xFF = egal), 0 wenn keins da ist.
     *   pci_bar gibt die Basisadresse von BAR n zurueck (64-Bit-BARs werden zusammengesetzt) und in *size
     *   (darf NULL sein) die Groesse. Mit map() einblenden, mit pci_enable() Memory-Zugriff und Bus-Master
     *   einschalten. Weitere Config-Register (z.B. 0x3C = Interrupt-Leitung) per pci_read32/pci_write32.
     * Interrupts: irq_route leitet einen ISA-IRQ (mit ACPI-Overrides) an einen Vektor, irq_route_gsi eine
     *   beliebige IOAPIC-Leitung (PCI-INTx ist pegelgesteuert, aktiv niedrig: level_low = 1). Den Handler traegt
     *   man mit set_handler ein. Er MUSS am Ende irq_eoi() rufen. Vektoren 0x20-0x2F und 0x81/0x82 sind belegt.
     * Speichergeraete: blk_register meldet ein BlockDev an (blk.h), blk_scan erkennt Partitionen und
     *   Dateisysteme darauf, automount bindet neue FAT32/NTFS-Partitionen ein (schon eingebundene bleiben).
     * kbd_inject: ein Zeichen in den Tastaturpuffer der Shell legen (z.B. von einem USB-HID-Treiber).
     *   0 = ok, -1 = Puffer voll. */
    int            (*pci_count)(void);
    const PciDev  *(*pci_get)(int index);
    const PciDev  *(*pci_find_class)(uint8_t cls, uint8_t sub, uint8_t prog, int index);
    uint32_t       (*pci_read32)(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
    void           (*pci_write32)(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
    void           (*pci_enable)(const PciDev *d);
    uint64_t       (*pci_bar)(const PciDev *d, int n, uint64_t *size);
    int            (*irq_route)(uint8_t isa_irq, uint8_t vector);
    int            (*irq_route_gsi)(uint32_t gsi, uint8_t vector, int level_low);
    void           (*irq_eoi)(void);
    int            (*blk_register)(BlockDev *d);
    void           (*blk_scan)(BlockDev *disk);
    void           (*automount)(void);
    int            (*kbd_inject)(char c);

    /* --- Shell ersetzen (ab Version 7) ---
     * shell_stop:  beendet die eingebaute Shell, bzw. verhindert, dass sie beim Boot startet (STRT-Programme
     *              laufen vor ihr). Wer das ruft, muss selbst eine Eingabe anbieten. Kommt man mit einem kaputten
     *              Ersatz nicht mehr rein: im Boot-Menue "ohne Module" waehlen, das ueberspringt auch C:\Start.
     * shell_start: startet die eingebaute Shell wieder (z.B. beim "exit" der Ersatz-Shell). 0 = ok.
     * file_stat / file_list: Eintrag bzw. Ordnerinhalt (FsEntry, cb != 0 beendet das Auflisten).
     * cur_drive / cur_path / set_drive / chdir: Arbeitsverzeichnis, das auch exec_run und die Dateifunktionen
     *              fuer relative Pfade benutzen. cur_drive liefert 0, wenn kein Laufwerk eingebunden ist.
     * strerror:    Text zu einem Fehlercode (FS_E* und die des Loaders). con_clear / con_backspace: Konsole. */
    int            (*shell_stop)(void);
    int            (*shell_start)(void);
    int            (*file_stat)(const char *path, FsEntry *e);
    int            (*file_list)(const char *path, FsDirCb cb, void *ctx);
    char           (*cur_drive)(void);
    const char    *(*cur_path)(void);
    int            (*set_drive)(char letter);
    int            (*chdir)(const char *path);
    const char    *(*strerror)(int err);
    void           (*con_clear)(void);
    void           (*con_backspace)(void);

    /* --- Laufwerke, Zellkonsole und Editor (ab Version 8) ---
     * Alle Dateifunktionen laufen ueber die VFS-Schicht und damit auf FAT32 wie NTFS gleich.
     * file_set_rw: Schreibschutz eines eingebundenen Laufwerks (A..Z) umschalten (Standard: nur lesen).
     * file_mount / file_unmount: Blockgeraet (z.B. "nvme0p1") als Laufwerk einbinden / loesen.
     * file_statfs: Gesamt- und freie Bytes; letter 0 = aktuelles Laufwerk. drive_info: siehe DriveInfo
     *   (letter 0 = aktuelles Laufwerk, FS_ENODRV wenn nichts eingebunden ist).
     * file_copy: Datei kopieren; ist das Ziel ein Ordner, bleibt der Name; *bytes (darf NULL sein) = Groesse.
     * con_size / con_put_at: Zellzugriff fuer Vollbild-Programme (nur mit Framebuffer; sonst -1). con_put_at(x, y,
     *   zeichen, fg, bg) schreibt in die nutzbare Flaeche, Farben als 0xRRGGBB, und zeichnet nur Aenderungen.
     * edit: Editor starten (hex = 1: Hex-Ansicht); kehrt zurueck, wenn der Benutzer ihn beendet. */
    int            (*file_set_rw)(char letter, int rw);
    int            (*file_mount)(const char *dev, char letter, int rw);
    int            (*file_unmount)(char letter);
    int            (*file_statfs)(char letter, uint64_t *total, uint64_t *free_bytes);
    int            (*file_copy)(const char *src, const char *dst, uint64_t *bytes);
    int            (*drive_info)(char letter, DriveInfo *out);
    int            (*con_size)(int *w, int *h);
    void           (*con_put_at)(int x, int y, char c, uint32_t fg_rgb, uint32_t bg_rgb);
    int            (*edit)(const char *path, int hex);

    /* --- Tastatur-Treiber (ab Version 9) ---
     * kbd_scancode: PS/2-Scancode (Set 1) einspeisen. Erweiterte Tasten brauchen vorher 0xE0, Loslassen ist der
     *   Code | 0x80. Layout (DE/US), Umschalt, AltGr, Strg und Feststelltaste bearbeitet der Kernel wie bei PS/2,
     *   ein USB-Treiber muss also nur HID-Tasten in Scancodes uebersetzen (und selbst wiederholen). */
    void           (*kbd_scancode)(uint8_t sc);

    /* --- Sprache (ab Version 10) ---
     * lang_get / lang_set: 0 = Deutsch, 1 = Englisch. Texte, die ein Programm mit kapi.puts ausgibt, werden uebersetzt, wenn
     * sie als deutscher Originaltext in kernel/lang_en.inc stehen. */
    int            (*lang_get)(void);
    void           (*lang_set)(int lang);
} KernelApi;

extern KernelApi kapi;

#endif /* BIYOS_KAPI_H */
