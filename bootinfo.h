/*
 * bootinfo.h - Uebergabestruktur zwischen Bootloader und Kernel.
 * Wird von beiden Seiten eingebunden. Alle Felder haben feste Breite.
 *
 * Kernel-Einstieg (System-V-ABI): void kmain(BootInfo *bi);   -> bi liegt in rdi
 * Der Kernel laeuft beim Einsprung auf dem UEFI-Stack mit Identity-Mapping und
 * sollte so frueh wie moeglich auf einen eigenen Stack und eigene Seitentabellen wechseln.
 */
#ifndef BIYOS_BOOTINFO_H
#define BIYOS_BOOTINFO_H

#include <stdint.h>

#define BIYOS_BOOTINFO_MAGIC   0x4249594F53494E46ULL   /* "BIYOSINF" */
#define BIYOS_BOOTINFO_VERSION 1

#define BOOT_MODE_NORMAL       0   /* alle Module aus boot.cfg laden */
#define BOOT_MODE_NO_MODULES   1   /* ohne Module booten */
#define BOOT_MODE_SELECTED     2   /* nur die im Menue gewaehlten Module */

#define FB_FORMAT_RGBX 0
#define FB_FORMAT_BGRX 1
#define FB_FORMAT_OTHER 2          /* Bitmask oder kein Framebuffer */

typedef struct {
    char     name[48];             /* Dateiname des Moduls (ohne Pfad) */
    uint64_t addr;                 /* physische Adresse der geladenen Datei */
    uint64_t size;                 /* Groesse in Bytes */
} BootModule;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t boot_mode;

    uint64_t fb_base;              /* 0, falls kein Framebuffer */
    uint64_t fb_size;
    uint32_t fb_width, fb_height;
    uint32_t fb_pitch;             /* Bytes pro Zeile */
    uint32_t fb_format;

    uint64_t mmap;                 /* Zeiger auf UEFI-Speicherkarte */
    uint64_t mmap_size;            /* Groesse in Bytes */
    uint64_t mmap_desc_size;       /* Groesse eines Deskriptors (nicht sizeof!) */
    uint32_t mmap_desc_version;
    uint32_t _pad0;

    uint64_t rsdp;                 /* ACPI-RSDP, 0 falls nicht gefunden */

    uint64_t kernel_base;          /* niedrigste/hoechste Adresse der Kernel-Segmente */
    uint64_t kernel_end;

    uint64_t module_count;
    uint64_t modules;              /* Zeiger auf BootModule[module_count] */
} BootInfo;

#endif /* BIYOS_BOOTINFO_H */
