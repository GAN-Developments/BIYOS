/* SPDX-License-Identifier: MIT */
/* Host-Testprogramm fuer kernel/mkfs.c: schreibt NTFS, FAT32 oder GPT in eine Abbilddatei, um das Ergebnis mit ntfs-3g,
 * dosfstools und sgdisk zu pruefen.
 *   gcc -O1 -Wall -I kernel -o /tmp/mkfs-host tools/mkfs-host.c
 *   mkfs-host ntfs  img <start_lba> <sektoren> <sektorgroesse> [label]
 *   mkfs-host fat32 img <start_lba> <sektoren> <sektorgroesse> [label]
 *   mkfs-host gpt   img <sektoren> <sektorgroesse> <esp_sektoren>   (ESP + NTFS-Datenpartition) */
#include "../kernel/mkfs.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fd_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    struct { FILE *f; uint32_t ss; uint64_t base; } *c = ctx;
    if (fseeko(c->f, (off_t)((c->base + lba) * c->ss), SEEK_SET)) return -1;
    return fwrite(buf, c->ss, count, c->f) == count ? 0 : -1;
}
static void prog(void *ctx, const char *what, int pct) { (void)ctx; fprintf(stderr, "  %3d%% %s\n", pct, what); }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "Aufruf: siehe Kopfkommentar\n"); return 2; }
    const char *cmd = argv[1];
    FILE *f = fopen(argv[2], "r+b");
    if (!f) { perror("open"); return 1; }
    struct { FILE *f; uint32_t ss; uint64_t base; } c;
    c.f = f;
    MkDev d;
    memset(&d, 0, sizeof d);
    d.write = fd_write; d.ctx = &c; d.alloc = malloc; d.release = free; d.progress = prog;
    d.ntfs_time = 133800000000000000ULL; d.serial = 0x1122334455667788ULL;
    int r;
    if (!strcmp(cmd, "gpt")) {
        d.sectors = strtoull(argv[3], 0, 0); d.sector_size = (uint32_t)strtoul(argv[4], 0, 0); c.ss = d.sector_size; c.base = 0;
        uint64_t esp = strtoull(argv[5], 0, 0);
        uint64_t s1 = mkfs_align_mib(34, d.sector_size), n1 = esp;
        uint64_t s2 = mkfs_align_mib(s1 + n1, d.sector_size), last = d.sectors - 34 - 1;
        MkPart p[2];
        memset(p, 0, sizeof p);
        memcpy(p[0].type_guid, MK_GUID_ESP, 16); strcpy(p[0].name, "EFI System"); p[0].first_lba = s1; p[0].sectors = n1; memset(p[0].guid, 0x11, 16);
        memcpy(p[1].type_guid, MK_GUID_BASIC, 16); strcpy(p[1].name, "BIYOS Data"); p[1].first_lba = s2; p[1].sectors = last - s2 + 1; memset(p[1].guid, 0x22, 16);
        uint8_t dg[16]; memset(dg, 0x33, 16);
        r = mkfs_gpt(&d, p, 2, dg);
        printf("ESP: start %llu, %llu Sektoren; Daten: start %llu, %llu Sektoren\n", (unsigned long long)s1, (unsigned long long)n1, (unsigned long long)s2, (unsigned long long)p[1].sectors);
    } else {
        d.start_lba = strtoull(argv[3], 0, 0); d.sectors = strtoull(argv[4], 0, 0); d.sector_size = (uint32_t)strtoul(argv[5], 0, 0);
        c.ss = d.sector_size; c.base = d.start_lba;
        const char *label = argc > 6 ? argv[6] : "BIYOS";
        r = !strcmp(cmd, "ntfs") ? mkfs_ntfs(&d, label) : mkfs_fat32(&d, label);
    }
    fclose(f);
    printf("Ergebnis: %d\n", r);
    return r ? 1 : 0;
}
