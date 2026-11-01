/*
 * blk.h - Blockgeraete: ganze Datenträger und Partitionen
 *
 * Treiber (nvme.c, spaeter AHCI/USB) registrieren ein BlockDev. blk.c legt fuer jede Partition
 * einer GPT/MBR ein weiteres BlockDev an, das auf das Elterngeraet verweist. Dateisysteme
 * arbeiten spaeter nur gegen BlockDev und kennen den Treiber nicht.
 *
 * Alle Funktionszeiger sind austauschbar (kein Schutz): ein Programm kann read/write eines
 * Geraets umbiegen, z.B. um Zugriffe mitzuschneiden oder ein Image einzublenden.
 */
#ifndef BIYOS_BLK_H
#define BIYOS_BLK_H

#include <stdint.h>

typedef struct BlockDev {
    char name[16];           /* z.B. "nvme0", "nvme0p1" */
    uint64_t sectors;        /* Anzahl Sektoren */
    uint32_t sector_size;    /* Bytes pro Sektor (512 oder 4096) */
    int (*read)(struct BlockDev *d, uint64_t lba, uint32_t count, void *buf);          /* 0 = ok */
    int (*write)(struct BlockDev *d, uint64_t lba, uint32_t count, const void *buf);   /* 0 = ok */
    void *priv;
    struct BlockDev *parent; /* 0 bei ganzen Datentraegern */
    uint64_t first_lba;      /* Start im Elterngeraet (bei Partitionen) */
    char type[24];           /* Partitionstyp, z.B. "EFI SYSTEM" */
    char fs[8];              /* erkanntes Dateisystem: NTFS, FAT32, FAT16, EXFAT oder leer */
} BlockDev;

#endif /* BIYOS_BLK_H */
