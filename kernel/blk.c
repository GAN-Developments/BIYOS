/* SPDX-License-Identifier: MIT */
/*
 * blk.c - Blockgeraete-Register, Partitionserkennung (GPT und MBR), Dateisystem-Erkennung
 */
#include "kernel.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#define MAX_BLK 24

static BlockDev *devs[MAX_BLK];
static int ndevs;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int blk_register(BlockDev *d) {
    if (ndevs >= MAX_BLK) return -1;
    devs[ndevs++] = d;
    return 0;
}

int blk_count(void) { return ndevs; }

BlockDev *blk_get(int i) { return (i >= 0 && i < ndevs) ? devs[i] : 0; }

BlockDev *blk_find(const char *name) {
    for (int i = 0; i < ndevs; i++) {
        const char *a = devs[i]->name, *b = name;
        while (*a && lower(*a) == lower(*b)) {
            a++;
            b++;
        }
        if (*a == *b) return devs[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Partitionen                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    BlockDev *parent;
    uint64_t first;
} PartPriv;

static int part_read(BlockDev *d, uint64_t lba, uint32_t n, void *buf) {
    PartPriv *p = d->priv;
    if (lba + n > d->sectors) return -3;
    return p->parent->read(p->parent, p->first + lba, n, buf);
}

static int part_write(BlockDev *d, uint64_t lba, uint32_t n, const void *buf) {
    PartPriv *p = d->priv;
    if (lba + n > d->sectors) return -3;
    return p->parent->write(p->parent, p->first + lba, n, buf);
}

static void scopy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int memeq(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    while (n--)
        if (*p++ != *q++) return 0;
    return 1;
}

/* Dateisystem am Anfang einer Partition erkennen */
static void detect_fs(BlockDev *d) {
    d->fs[0] = 0;
    uint8_t *s = kapi.malloc(d->sector_size);
    if (!s) return;
    if (d->read(d, 0, 1, s) == 0) {
        if (memeq(s + 3, "NTFS    ", 8)) scopy(d->fs, "NTFS", 8);
        else if (memeq(s + 3, "EXFAT   ", 8)) scopy(d->fs, "EXFAT", 8);
        else if (memeq(s + 3, "-FVE-FS-", 8)) scopy(d->fs, "BITLCK", 8);   /* BitLocker: verschluesselt */
        else if (memeq(s + 0x52, "FAT32   ", 8)) scopy(d->fs, "FAT32", 8);
        else if (memeq(s + 0x36, "FAT16   ", 8)) scopy(d->fs, "FAT16", 8);
        else if (memeq(s + 0x36, "FAT12   ", 8)) scopy(d->fs, "FAT12", 8);
    }
    kapi.free(s);
}

static void add_partition(BlockDev *disk, int number, uint64_t first, uint64_t count, const char *type) {
    BlockDev *p = kapi.malloc(sizeof(BlockDev));
    PartPriv *pp = kapi.malloc(sizeof(PartPriv));
    if (!p || !pp) {
        kapi.free(p);
        kapi.free(pp);
        return;
    }
    memset(p, 0, sizeof(BlockDev));
    pp->parent = disk;
    pp->first = first;

    int n = 0;
    for (; disk->name[n] && n < 12; n++) p->name[n] = disk->name[n];
    p->name[n++] = 'p';
    if (number >= 10) p->name[n++] = (char)('0' + number / 10);
    p->name[n++] = (char)('0' + number % 10);
    p->name[n] = 0;

    p->sectors = count;
    p->sector_size = disk->sector_size;
    p->read = part_read;
    p->write = part_write;
    p->priv = pp;
    p->parent = disk;
    p->first_lba = first;
    scopy(p->type, type, sizeof p->type);
    detect_fs(p);
    blk_register(p);
}

typedef struct {
    uint8_t guid[16];
    const char *name;
} GuidName;

static const GuidName known[] = {
    {{0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}, "EFI SYSTEM"},
    {{0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}, "WINDOWS DATEN"},
    {{0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D, 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}, "WINDOWS RESERVIERT"},
    {{0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D, 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}, "WINDOWS RECOVERY"},
    {{0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}, "LINUX DATEN"},
    {{0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E, 0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49}, "BIOS BOOT"},
};

static const char *guid_type(const uint8_t *g) {
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (memeq(known[i].guid, g, 16)) return known[i].name;
    return "ANDERER TYP";
}

static const char *mbr_type(uint8_t t) {
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/EXFAT";
    case 0x82: return "LINUX SWAP";
    case 0x83: return "LINUX DATEN";
    case 0xEF: return "EFI SYSTEM";
    default: return "MBR-TYP";
    }
}

/* Partitionen eines Datentraegers suchen (GPT, sonst MBR) und als BlockDev registrieren */
void blk_scan_partitions(BlockDev *disk) {
    uint32_t ss = disk->sector_size;
    uint8_t *s = kapi.malloc(ss);
    if (!s) return;

    int found_gpt = 0;
    if (disk->read(disk, 1, 1, s) == 0 && memeq(s, "EFI PART", 8)) {
        uint64_t entries_lba = *(uint64_t *)(s + 72);
        uint32_t num = *(uint32_t *)(s + 80);
        uint32_t esize = *(uint32_t *)(s + 84);
        if (num > 256) num = 256;
        if (esize >= 128 && esize <= 1024) {
            uint32_t bytes = num * esize;
            uint32_t sectors = (bytes + ss - 1) / ss;
            uint8_t *tab = kapi.malloc((size_t)sectors * ss);
            if (tab && disk->read(disk, entries_lba, sectors, tab) == 0) {
                found_gpt = 1;
                for (uint32_t i = 0; i < num; i++) {
                    const uint8_t *e = tab + (size_t)i * esize;
                    int empty = 1;
                    for (int k = 0; k < 16; k++)
                        if (e[k]) empty = 0;
                    if (empty) continue;
                    uint64_t first = *(const uint64_t *)(e + 32), last = *(const uint64_t *)(e + 40);
                    if (last < first || last >= disk->sectors) continue;
                    add_partition(disk, (int)i + 1, first, last - first + 1, guid_type(e));
                }
            }
            kapi.free(tab);
        }
    }

    if (!found_gpt && disk->read(disk, 0, 1, s) == 0 && s[510] == 0x55 && s[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = s + 0x1BE + 16 * i;
            uint8_t type = e[4];
            uint32_t first = *(const uint32_t *)(e + 8), count = *(const uint32_t *)(e + 12);
            if (!type || !count || type == 0xEE || type == 0x05 || type == 0x0F) continue;   /* erweiterte: spaeter */
            if ((uint64_t)first + count > disk->sectors) continue;
            add_partition(disk, i + 1, first, count, mbr_type(type));
        }
    }
    kapi.free(s);
}

/* ------------------------------------------------------------------ */
void storage_init(void) {
    pci_init();
    con_puts("PCI: ");
    con_dec((uint64_t)pci_count());
    con_puts(" GERAETE\n");
    if (nvme_init() <= 0) con_puts("STORAGE: KEIN NVME-LAUFWERK\n");
    con_puts("BLOCKGERAETE: ");
    con_dec((uint64_t)blk_count());
    con_puts("\n");
}
