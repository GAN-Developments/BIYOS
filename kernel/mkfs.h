/* SPDX-License-Identifier: MIT */
/*
 * mkfs.h - Formatierer fuer NTFS und FAT32 sowie GPT-Partitionstabellen
 *
 * mkfs.c hat keine Abhaengigkeit vom Kernel: alles laeuft ueber MkDev (Schreib-Callback, Speicher, Uhrzeit). Dadurch laesst
 * sich derselbe Code im Kernel (install.c) und als Host-Programm (tools/mkfs-host.c) bauen und dort gegen ntfs-3g,
 * dosfstools und sgdisk pruefen.
 */
#ifndef BIYOS_MKFS_H
#define BIYOS_MKFS_H

#include <stddef.h>
#include <stdint.h>

#define MKFS_OK        0
#define MKFS_EIO      -1   /* Schreibfehler */
#define MKFS_ETOOSMALL -2  /* Ziel zu klein fuer dieses Dateisystem */
#define MKFS_ENOMEM   -3
#define MKFS_EPARAM   -4   /* Sektorgroesse oder Groesse nicht unterstuetzt */

typedef struct MkDev {
    int      (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);   /* 0 = ok */
    void      *ctx;
    uint64_t   sectors;         /* Groesse des Ziels in Sektoren */
    uint32_t   sector_size;     /* 512 oder 4096 */
    uint64_t   start_lba;       /* Lage auf dem Datentraeger (fuer "versteckte Sektoren" im Bootsektor) */
    void      *(*alloc)(size_t);
    void      (*release)(void *);
    uint64_t   ntfs_time;       /* aktuelle Zeit in 100-ns-Schritten seit 1601-01-01 */
    uint64_t   serial;          /* Volume-Seriennummer */
    void      (*progress)(void *ctx, const char *what, int percent);   /* darf 0 sein */
} MkDev;

int mkfs_ntfs(MkDev *d, const char *label);
int mkfs_fat32(MkDev *d, const char *label);

/* GPT: Partitionen mit Start/Laenge in Sektoren, Typ-GUID und Name */
typedef struct {
    uint64_t first_lba, sectors;
    uint8_t  type_guid[16];     /* gemischt-endian, wie auf dem Datentraeger */
    char     name[36];          /* ASCII */
    uint8_t  guid[16];          /* eindeutige Partitions-GUID (der Aufrufer fuellt sie) */
} MkPart;

extern const uint8_t MK_GUID_ESP[16];     /* EFI System Partition */
extern const uint8_t MK_GUID_BASIC[16];   /* Microsoft Basic Data (NTFS/FAT) */

int mkfs_gpt(MkDev *disk, const MkPart *parts, int nparts, const uint8_t disk_guid[16]);

/* Hilfsfunktion: erste nutzbare Position (1-MiB-Ausrichtung) */
uint64_t mkfs_align_mib(uint64_t lba, uint32_t sector_size);

#endif
