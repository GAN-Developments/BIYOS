/* SPDX-License-Identifier: MIT */
/*
 * install.c - Installer und Formatierbefehl
 *
 * Der Installer laeuft im Kernel (Befehl "install", oder automatisch, wenn die boot.cfg install=1 enthaelt, z.B. auf der ISO).
 * Er legt auf einem gewaehlten Datentraeger eine GPT an mit
 *     1. EFI-Systempartition (FAT32, 128 MiB; 260 MiB bei 4096-Byte-Sektoren) mit BIYOS (bootbar per UEFI-Fallbackpfad
 *        \EFI\BOOT\BOOTX64.EFI), und
 *     2. einer NTFS-Datenpartition (Rest des Datentraegers oder eine gewaehlte Groesse),
 * formatiert beide (kernel/mkfs.c) und kopiert die Dateien, die der Bootloader als Nutzlast (payload= in boot.cfg) in den
 * Speicher geladen hat. Die boot.cfg der Zielplatte erzeugt der Installer selbst (Sprache und Layout wie gewaehlt).
 *
 * "mkfs ntfs|fat32 <geraet> [name] -y" formatiert eine einzelne Partition.
 */
#include "kernel.h"
#include "mkfs.h"

/* ------------------------------------------------------------------ */
/* Uhrzeit (CMOS-RTC) und Zufall                                       */
/* ------------------------------------------------------------------ */
static uint8_t cmos(uint8_t reg) {
    __asm__ volatile("outb %0, $0x70" ::"a"(reg));
    uint8_t v;
    __asm__ volatile("inb $0x71, %0" : "=a"(v));
    return v;
}

static unsigned bcd(unsigned v, int is_bin) { return is_bin ? v : (v & 15) + (v >> 4) * 10; }

/* aktuelle Zeit in 100-ns-Schritten seit 1601-01-01 (NTFS-Format); ohne brauchbare RTC ein fester Wert */
static uint64_t ntfs_now(void) {
    for (int tries = 0; tries < 1000 && (cmos(0x0A) & 0x80); tries++) {}   /* Aktualisierung abwarten */
    int is_bin = (cmos(0x0B) & 4) != 0;
    unsigned sec = bcd(cmos(0x00), is_bin), min = bcd(cmos(0x02), is_bin);
    unsigned hr = cmos(0x04);
    int pm = hr & 0x80;
    hr = bcd(hr & 0x7F, is_bin);
    if (!(cmos(0x0B) & 2) && pm) hr = (hr % 12) + 12;   /* 12-Stunden-Modus */
    unsigned day = bcd(cmos(0x07), is_bin), mon = bcd(cmos(0x08), is_bin), yr = bcd(cmos(0x09), is_bin);
    unsigned cen = bcd(cmos(0x32), is_bin);
    unsigned year = (cen >= 19 && cen <= 21 ? cen * 100 : 2000) + yr;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || year < 2020) return 133800000000000000ULL;
    static const unsigned cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    uint64_t days = 0;
    for (unsigned y = 1601; y < year; y++) days += ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 366 : 365;
    days += cum[mon - 1] + (day - 1);
    if (mon > 2 && ((year % 4 == 0 && year % 100) || year % 400 == 0)) days++;
    uint64_t secs = days * 86400 + hr * 3600 + min * 60 + sec;
    return secs * 10000000ULL;
}

static uint64_t rng_state;

static uint64_t rnd(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    rng_state ^= (((uint64_t)hi << 32) | lo) + 0x9E3779B97F4A7C15ULL;
    rng_state ^= rng_state << 13;   /* xorshift64 */
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static void make_guid(uint8_t g[16]) {
    uint64_t a = rnd(), b = rnd();
    for (int i = 0; i < 8; i++) {
        g[i] = (uint8_t)(a >> (8 * i));
        g[8 + i] = (uint8_t)(b >> (8 * i));
    }
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* Version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

/* ------------------------------------------------------------------ */
/* Anbindung des Formatierers an BlockDev                              */
/* ------------------------------------------------------------------ */
static int mk_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    BlockDev *b = ctx;
    return b->write(b, lba, count, buf);
}

static void *mk_alloc(size_t n) { return kapi.malloc(n); }
static void mk_release(void *p) { kapi.free(p); }

static void mk_progress(void *ctx, const char *what, int pct) {
    (void)ctx;
    (void)what;
    con_putc('.');
    (void)pct;
}

static void mkdev_for(MkDev *d, BlockDev *b) {
    d->write = mk_write;
    d->ctx = b;
    d->sectors = b->sectors;
    d->sector_size = b->sector_size;
    d->start_lba = blk_part_start(b);
    d->alloc = mk_alloc;
    d->release = mk_release;
    d->ntfs_time = ntfs_now();
    d->serial = rnd();
    d->progress = mk_progress;
}

static const char *mk_err(int r) {
    switch (r) {
    case MKFS_EIO: return "SCHREIBFEHLER AUF DEM DATENTRAEGER";
    case MKFS_ETOOSMALL: return "PARTITION ZU KLEIN FUER DIESES DATEISYSTEM";
    case MKFS_ENOMEM: return "KEIN SPEICHER";
    default: return "SEKTORGROESSE NICHT UNTERSTUETZT";
    }
}

/* ------------------------------------------------------------------ */
/* Eingabe                                                             */
/* ------------------------------------------------------------------ */
static int getkey_wait(void) {
    int k;
    while ((k = kbd_getc()) < 0) kapi.yield();
    return k;
}

static void getline(char *line, int max) {
    int n = 0;
    for (;;) {
        int c = getkey_wait();
        if (c == '\n') {
            con_putc('\n');
            line[n] = 0;
            return;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                con_backspace();
            }
        } else if (c >= 32 && c < 127 && n < max - 1) {
            line[n++] = (char)c;
            con_putc((char)c);
        }
    }
}

static int streq_ci(const char *a, const char *b) {
    while (*a && ((*a | 32) == (*b | 32))) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t parse_dec(const char *s, int *ok) {
    uint64_t v = 0;
    *ok = s[0] != 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            *ok = 0;
            return 0;
        }
        v = v * 10 + (uint64_t)(*s - '0');
    }
    return v;
}

static int find_free_letter(void) {
    for (char l = 'Z'; l >= 'D'; l--)
        if (!vfs_get(l)) return l;
    return 0;
}

/* ------------------------------------------------------------------ */
/* mkfs <typ> <geraet> [name] -y                                       */
/* ------------------------------------------------------------------ */
int cmd_mkfs(int argc, char **argv) {
    if (argc < 3) {
        con_puts("MKFS NTFS|FAT32 <GERAET> [NAME] -Y\n");
        return 0;
    }
    int yes = 0;
    const char *label = 0;
    for (int i = 3; i < argc; i++) {
        if (streq_ci(argv[i], "-y")) yes = 1;
        else label = argv[i];
    }
    BlockDev *b = blk_find(argv[2]);
    if (!b) {
        con_puts("UNBEKANNTES GERAET (DISKS)\n");
        return 0;
    }
    int ntfs = streq_ci(argv[1], "ntfs");
    if (!ntfs && !streq_ci(argv[1], "fat32")) {
        con_puts("MKFS NTFS|FAT32 <GERAET> [NAME] -Y\n");
        return 0;
    }
    for (char l = 'A'; l <= 'Z'; l++) {
        Fs *fs = vfs_get(l);
        if (fs && fs->dev == b) {
            con_puts("GERAET IST EINGEBUNDEN: ZUERST AUSHAENGEN (MOUNT)\n");
            return 0;
        }
    }
    if (!yes) {
        con_puts("ALLE DATEN AUF DIESEM GERAET GEHEN VERLOREN. ZUM BESTAETIGEN -Y ANHAENGEN.\n");
        return 0;
    }
    MkDev d;
    mkdev_for(&d, b);
    con_puts("FORMATIERE");
    int r = ntfs ? mkfs_ntfs(&d, label ? label : "BIYOS") : mkfs_fat32(&d, label ? label : "BIYOS");
    con_putc('\n');
    if (r) {
        con_puts("FEHLER: ");
        con_puts(mk_err(r));
        con_putc('\n');
    } else {
        con_puts("FERTIG\n");
        blk_scan_partitions(b->parent ? b->parent : b);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Installer                                                           */
/* ------------------------------------------------------------------ */
static void cfg_append(char *buf, size_t *n, size_t max, const char *s) {
    while (*s && *n + 1 < max) buf[(*n)++] = *s++;
    buf[*n] = 0;
}

static int has_prefix(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static int ends_with(const char *s, const char *suffix) {
    size_t a = 0, b = 0;
    while (s[a]) a++;
    while (suffix[b]) b++;
    if (b > a) return 0;
    for (size_t i = 0; i < b; i++)
        if (s[a - b + i] != suffix[i]) return 0;
    return 1;
}

/* Verzeichnisse eines Pfades "/A/B/datei" anlegen ("X:/A", "X:/A/B"); bereits vorhandene sind kein Fehler */
static void make_parents(char drive, const char *path) {
    char p[96];
    size_t n = 0;
    p[n++] = drive;
    p[n++] = ':';
    for (size_t i = 0; path[i] && n < sizeof(p) - 2; i++) {
        if (path[i] == '/' && i > 0) {
            p[n] = 0;
            vfs_mkdir(p);
        }
        p[n++] = path[i];
    }
}

static int copy_payload(char drive, int lang, int layout) {
    const BootModule *pl = (const BootModule *)(uintptr_t)g_bi->payloads;
    char cfg[1024];
    size_t cn = 0;
    cfg[0] = 0;
    cfg_append(cfg, &cn, sizeof(cfg), "# BIYOS Boot-Konfiguration (vom Installer erzeugt)\nkernel=/BIYOS/kernel.elf\n\n");
    cfg_append(cfg, &cn, sizeof(cfg), lang ? "lang=en\n" : "lang=de\n");
    cfg_append(cfg, &cn, sizeof(cfg), layout ? "layout=us\n\n" : "layout=de\n\n");
    for (uint64_t i = 0; i < g_bi->payload_count; i++)   /* Module in der Reihenfolge der Nutzlast eintragen */
        if (has_prefix(pl[i].name, "/BIYOS/modules/") && ends_with(pl[i].name, ".drv")) {
            cfg_append(cfg, &cn, sizeof(cfg), "module=");
            cfg_append(cfg, &cn, sizeof(cfg), pl[i].name);
            cfg_append(cfg, &cn, sizeof(cfg), "\n");
        }
    for (uint64_t i = 0; i < g_bi->payload_count; i++) {
        const char *name = pl[i].name;
        if (ends_with(name, "boot.cfg")) continue;   /* wird oben neu erzeugt */
        char path[96];
        size_t n = 0;
        path[n++] = drive;
        path[n++] = ':';
        for (size_t k = 0; name[k] && n < sizeof(path) - 1; k++) path[n++] = name[k];
        path[n] = 0;
        make_parents(drive, name);
        int r = vfs_write(path, (const void *)(uintptr_t)pl[i].addr, pl[i].size, 0);
        con_puts("  ");
        con_puts(path);
        if (r) {
            con_puts(" FEHLER: ");
            con_puts(exec_strerror(r));
            con_putc('\n');
            return r;
        }
        con_putc('\n');
    }
    char cpath[24] = {drive, ':', '/', 'B', 'I', 'Y', 'O', 'S', '/', 'b', 'o', 'o', 't', '.', 'c', 'f', 'g', 0};
    int r = vfs_write(cpath, cfg, cn, 0);
    con_puts("  ");
    con_puts(cpath);
    con_putc('\n');
    return r;
}

static uint64_t mib(uint64_t sectors, uint32_t ss) { return sectors * ss / (1024 * 1024); }

/* Partition p (1, 2, ...) eines Datentraegers finden: nvme0 -> nvme0p1, sata0 -> sata0p1 */
static BlockDev *find_part(BlockDev *disk, int p) {
    char pn[24];
    size_t n = 0;
    for (const char *s = disk->name; *s && n < 18; s++) pn[n++] = *s;
    if (n && disk->name[n - 1] >= '0' && disk->name[n - 1] <= '9') pn[n++] = 'p';
    pn[n++] = (char)('0' + p);
    pn[n] = 0;
    return blk_find(pn);
}

static int pick_disk(BlockDev **out) {
    BlockDev *disks[16];
    int nd = 0;
    con_puts("\nDATENTRAEGER:\n");
    for (int i = 0; i < blk_count() && nd < 16; i++) {
        BlockDev *b = blk_get(i);
        if (b->parent) continue;
        disks[nd++] = b;
        con_puts("  [");
        con_dec((uint64_t)nd);
        con_puts("]  ");
        con_puts(b->name);
        con_puts("  ");
        con_dec(mib(b->sectors, b->sector_size));
        con_puts(" MIB\n");
    }
    if (!nd) {
        con_puts("KEIN DATENTRAEGER GEFUNDEN\n");
        return -1;
    }
    for (;;) {
        con_puts("ZIELDATENTRAEGER (NUMMER, Q = ABBRECHEN): ");
        char line[16];
        getline(line, sizeof(line));
        if (line[0] == 'q' || line[0] == 'Q') return -1;
        int ok;
        uint64_t v = parse_dec(line, &ok);
        if (ok && v >= 1 && v <= (uint64_t)nd) {
            *out = disks[v - 1];
            return 0;
        }
    }
}

void install_run(void) {
    con_puts("\n=== BIYOS INSTALLER ===\n\n");
    if (!g_bi || !g_bi->payload_count) {
        con_puts("KEINE INSTALLATIONSDATEIEN (NUTZLAST) VORHANDEN. DER INSTALLER BRAUCHT DIE ISO ODER EINE BOOT.CFG MIT PAYLOAD=.\n");
        return;
    }
    /* Sprache und Tastaturlayout */
    con_puts("SPRACHE / LANGUAGE:  [1] DEUTSCH  [2] ENGLISH: ");
    char line[32];
    for (;;) {
        getline(line, sizeof(line));
        if (line[0] == '1') lang_set(0);
        else if (line[0] == '2') lang_set(1);
        else continue;
        break;
    }
    con_puts("TASTATUR:  [1] DEUTSCH (QWERTZ)  [2] US (QWERTY): ");
    for (;;) {
        getline(line, sizeof(line));
        if (line[0] == '1') kbd_set_layout(1);
        else if (line[0] == '2') kbd_set_layout(0);
        else continue;
        break;
    }
    int lang = g_lang, layout = kbd_layout_de() ? 0 : 1;

    BlockDev *disk;
    if (pick_disk(&disk) != 0) {
        con_puts("ABGEBROCHEN\n");
        return;
    }

    /* Layout der Partitionen */
    uint32_t ss = disk->sector_size;
    uint64_t esp_bytes = ss == 512 ? 128ULL << 20 : 260ULL << 20;
    uint64_t esp_start = mkfs_align_mib(2 + 16384 / ss, ss);
    uint64_t esp_sectors = esp_bytes / ss;
    uint64_t last_usable = disk->sectors - 1 - 16384 / ss - 1;
    uint64_t data_start = mkfs_align_mib(esp_start + esp_sectors, ss);
    uint64_t mib_sec = (1ULL << 20) / ss;
    uint64_t data_end = (last_usable + 1) / mib_sec * mib_sec;   /* auf 1 MiB abrunden */
    if (esp_start + esp_sectors > last_usable) {
        con_puts("DATENTRAEGER ZU KLEIN\n");
        return;
    }
    uint64_t data_sectors = data_end > data_start ? data_end - data_start : 0;
    if (data_sectors * ss < 32ULL << 20) data_sectors = 0;
    con_puts("\nGROESSE DER NTFS-DATENPARTITION IN MIB (EINGABETASTE = REST DES DATENTRAEGERS, 0 = KEINE): ");
    for (;;) {
        getline(line, sizeof(line));
        if (!line[0]) break;
        int ok;
        uint64_t m = parse_dec(line, &ok);
        if (!ok) continue;
        if (m == 0) {
            data_sectors = 0;
            break;
        }
        if (m * mib_sec <= data_sectors && m >= 32) {
            data_sectors = m * mib_sec;
            break;
        }
        con_puts("UNGUELTIG (MIN. 32, MAX. ");
        con_dec(mib(data_sectors, ss));
        con_puts("): ");
    }

    con_puts("\nALLE DATEN AUF ");
    con_puts(disk->name);
    con_puts(" WERDEN GELOESCHT.\n  EFI-PARTITION (FAT32): ");
    con_dec(mib(esp_sectors, ss));
    con_puts(" MIB\n");
    if (data_sectors) {
        con_puts("  DATENPARTITION (NTFS): ");
        con_dec(mib(data_sectors, ss));
        con_puts(" MIB\n");
    }
    con_puts("ZUM FORTFAHREN \"JA\" ODER \"YES\" EINGEBEN: ");
    getline(line, sizeof(line));
    if (!streq_ci(line, "ja") && !streq_ci(line, "yes")) {
        con_puts("ABGEBROCHEN\n");
        return;
    }

    /* GPT schreiben */
    MkDev dd;
    mkdev_for(&dd, disk);
    MkPart parts[2];
    for (int i = 0; i < 2; i++)
        for (size_t k = 0; k < sizeof(parts[i]); k++) ((uint8_t *)&parts[i])[k] = 0;
    for (int k = 0; k < 16; k++) parts[0].type_guid[k] = MK_GUID_ESP[k];
    parts[0].first_lba = esp_start;
    parts[0].sectors = esp_sectors;
    const char *n0 = "EFI SYSTEM", *n1 = "BIYOS DATA";
    for (int k = 0; n0[k]; k++) parts[0].name[k] = n0[k];
    make_guid(parts[0].guid);
    int np = 1;
    if (data_sectors) {
        for (int k = 0; k < 16; k++) parts[1].type_guid[k] = MK_GUID_BASIC[k];
        parts[1].first_lba = data_start;
        parts[1].sectors = data_sectors;
        for (int k = 0; n1[k]; k++) parts[1].name[k] = n1[k];
        make_guid(parts[1].guid);
        np = 2;
    }
    uint8_t dg[16];
    make_guid(dg);
    con_puts("\nPARTITIONSTABELLE ...");
    for (char l = 'A'; l <= 'Z'; l++) {   /* alles Eingebundene auf diesem Datentraeger aushaengen */
        Fs *fs = vfs_get(l);
        if (fs && fs->dev && (fs->dev == disk || fs->dev->parent == disk)) vfs_unmount(l);
    }
    int r = mkfs_gpt(&dd, parts, np, dg);
    if (r) {
        con_puts(" FEHLER: ");
        con_puts(mk_err(r));
        con_putc('\n');
        return;
    }
    blk_drop_partitions(disk);
    blk_scan_partitions(disk);
    con_puts(" OK\n");

    BlockDev *esp = find_part(disk, 1), *data = np == 2 ? find_part(disk, 2) : 0;
    if (!esp || (np == 2 && !data)) {
        con_puts("NEUE PARTITIONEN NICHT GEFUNDEN\n");
        return;
    }

    con_puts("FORMATIERE EFI-PARTITION (FAT32)");
    MkDev de;
    mkdev_for(&de, esp);
    r = mkfs_fat32(&de, "BIYOS");
    con_putc('\n');
    if (r) {
        con_puts("FEHLER: ");
        con_puts(mk_err(r));
        con_putc('\n');
        return;
    }
    if (np == 2) {
        con_puts("FORMATIERE DATENPARTITION (NTFS)");
        MkDev dn;
        mkdev_for(&dn, data);
        r = mkfs_ntfs(&dn, "BIYOSDATA");
        con_putc('\n');
        if (r) {
            con_puts("FEHLER: ");
            con_puts(mk_err(r));
            con_putc('\n');
            return;
        }
    }

    blk_drop_partitions(disk);   /* Dateisysteme erkennen lassen, jetzt wo die Partitionen formatiert sind */
    blk_scan_partitions(disk);
    esp = find_part(disk, 1);
    data = np == 2 ? find_part(disk, 2) : 0;
    if (!esp || (np == 2 && !data)) {
        con_puts("NEUE PARTITIONEN NICHT GEFUNDEN\n");
        return;
    }

    /* BIYOS auf die EFI-Partition kopieren */
    char dl = (char)find_free_letter();
    if (!dl) {
        con_puts("KEIN FREIER LAUFWERKSBUCHSTABE\n");
        return;
    }
    r = vfs_mount_dev(esp->name, dl, 1);
    if (r) {
        con_puts("EINBINDEN FEHLGESCHLAGEN: ");
        con_puts(exec_strerror(r));
        con_putc('\n');
        return;
    }
    con_puts("\nKOPIERE DATEIEN:\n");
    r = copy_payload(dl, lang, layout);
    vfs_unmount(dl);
    if (r) {
        con_puts("KOPIEREN FEHLGESCHLAGEN\n");
        return;
    }
    if (np == 2) {   /* C:\Start auf der Datenpartition anlegen (STRT-Programme dort starten beim Boot) */
        r = vfs_mount_dev(data->name, dl, 1);
        if (r == 0) {
            char sz[9] = {dl, ':', '/', 'S', 't', 'a', 'r', 't', 0};
            vfs_mkdir(sz);
            vfs_unmount(dl);
        }
    }
    kapi.sleep_ms(600);   /* Schreibcache der Platte (ahci.drv leert ihn 200 ms nach dem letzten Schreiben) */
    con_puts("\nINSTALLATION ABGESCHLOSSEN. INSTALLATIONSMEDIUM ENTFERNEN, DANN EINE TASTE DRUECKEN ZUM NEUSTART.\n");
    getkey_wait();
    shell_reboot();
}


void install_task(void *arg) {
    (void)arg;
    install_run();
    shell_start();   /* nach dem Installer (Abbruch) die Shell */
    kapi.task_exit();
}
