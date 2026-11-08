/* SPDX-License-Identifier: MIT */
/*
 * shell.c - eingebaute Notfall-Shell von BIYOS
 *
 * Laeuft als normale Task. Spaeter wird die eigentliche Shell ein STRT-Programm in C:\\Start;
 * diese hier bleibt als Rueckfallebene, falls keine startet.
 *
 * Bewusst ohne Schutz: peek/poke greifen direkt auf beliebige Adressen zu. Ein Zugriff auf
 * nicht gemappten Speicher endet mit einem Exception-Stopp.
 */
#include "kernel.h"

#define LINE_MAX 128
#define ARG_MAX  24

static size_t strlen_(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int streq_ci(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

static int parse_hex(const char *s, uint64_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return 0;
    uint64_t v = 0;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return 0;
        v = v * 16 + (uint64_t)d;
    }
    *out = v;
    return 1;
}

static int parse_num(const char *s, uint64_t *out) {   /* dezimal, oder hex mit 0x */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return parse_hex(s, out);
    if (!*s) return 0;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return 1;
}

static void pad_puts(const char *s, int w) {
    int n = 0;
    for (; s[n]; n++) con_putc(s[n]);
    for (; n < w; n++) con_putc(' ');
}

static void pad_dec(uint64_t v, int w) {
    int n = 1;
    for (uint64_t t = v; t >= 10; t /= 10) n++;
    for (; n < w; n++) con_putc(' ');
    con_dec(v);
}

static void hexn(uint64_t v, int digits) {
    static const char h[] = "0123456789ABCDEF";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) con_putc(h[(v >> i) & 0xF]);
}

/* ------------------------------------------------------------------ */
/* Befehle                                                             */
/* ------------------------------------------------------------------ */
static void cmd_help(void) {
    con_puts("BEFEHLE:\n");
    con_puts("  HELP                    DIESE LISTE\n");
    con_puts("  CLEAR                   BILDSCHIRM LEEREN\n");
    con_puts("  ECHO <TEXT>             TEXT AUSGEBEN\n");
    con_puts("  VER                     VERSION\n");
    con_puts("  MEM                     SPEICHER (PMM)\n");
    con_puts("  UPTIME                  LAUFZEIT\n");
    con_puts("  TASKS                   TASKLISTE\n");
    con_puts("  MODULE LIST|LOAD <DATEI.DRV>|REMOVE <NAME|NR>   KERNEL-MODULE (DRIV)\n");
    con_puts("  RUN <DATEI> [ARG..]     PROGRAMM (PLSE/STRT) AUSFUEHREN UND WARTEN\n");
    con_puts("  START <DATEI> [ARG..]   PROGRAMM ALS EIGENE TASK STARTEN\n");
    con_puts("  LAYOUT DE|US            TASTATURLAYOUT\n");
    con_puts("  PEEK <ADR> [LAENGE]     SPEICHER ANZEIGEN (HEX)\n");
    con_puts("  POKE <ADR> <WERT> [1|2|4|8]   SPEICHER SCHREIBEN (HEX)\n");
    con_puts("  MOUNT [<DEV> <X> [RW]]  LAUFWERKE (NUR LESEN, ES SEI DENN RW; NTFS-RW NUR BEI SAUBEREM VOLUME)\n");
    con_puts("  E:                      LAUFWERK WECHSELN\n");
    con_puts("  DIR|LS [PFAD]  CD|PWD [PFAD]  TYPE|CAT <DATEI>  DF\n");
    con_puts("  MKDIR <PFAD>  DEL|RM <PFAD>  REN|MV <VON> <NACH>  COPY|CP <VON> <NACH>\n");
    con_puts("  ECHO <TEXT> > <DATEI>   TEXT SCHREIBEN (>> HAENGT AN)\n");
    con_puts("  HEXWRITE 00 01 \"TEXT\" > <DATEI>   BYTES UND ASCII SCHREIBEN\n");
    con_puts("  PCI                     PCI-GERAETE\n");
    con_puts("  DISKS                   LAUFWERKE UND PARTITIONEN\n");
    con_puts("  DISKREAD <DEV> <LBA>    SEKTOR LESEN (ERSTE 256 BYTES)\n");
    con_puts("  REBOOT                  NEUSTART\n");
    con_puts("  SHUTDOWN                AUSSCHALTEN (BISHER NUR VM)\n");
}

static void cmd_mem(void) {
    con_puts("PMM: ");
    con_dec(pmm_free_page_count() >> 8);
    con_puts(" MIB FREI VON ");
    con_dec(pmm_total_pages() >> 8);
    con_puts(" MIB NUTZBAR (");
    con_dec(pmm_free_page_count());
    con_puts(" SEITEN FREI)\n");
}

static void cmd_uptime(void) {
    uint64_t ms = kapi.uptime_ms();
    con_dec(ms / 1000);
    con_puts(".");
    uint64_t frac = ms % 1000;
    if (frac < 100) con_putc('0');
    if (frac < 10) con_putc('0');
    con_dec(frac);
    con_puts(" S\n");
}

static void cmd_peek(int argc, char **argv) {
    uint64_t addr, len = 16;
    if (argc < 2 || !parse_hex(argv[1], &addr)) {
        con_puts("PEEK <ADRESSE> [LAENGE]\n");
        return;
    }
    if (argc > 2) parse_hex(argv[2], &len);
    if (len > 256) len = 256;
    for (uint64_t o = 0; o < len; o += 8) {
        hexn(addr + o, 16);
        con_puts(": ");
        for (uint64_t i = 0; i < 8 && o + i < len; i++) {
            hexn(*(volatile uint8_t *)(uintptr_t)(addr + o + i), 2);
            con_putc(' ');
        }
        con_putc('\n');
    }
}

static void cmd_poke(int argc, char **argv) {
    uint64_t addr, val, size = 1;
    if (argc < 3 || !parse_hex(argv[1], &addr) || !parse_hex(argv[2], &val)) {
        con_puts("POKE <ADRESSE> <WERT> [1|2|4|8]\n");
        return;
    }
    if (argc > 3) parse_hex(argv[3], &size);
    switch (size) {
    case 1: *(volatile uint8_t *)(uintptr_t)addr = (uint8_t)val; break;
    case 2: *(volatile uint16_t *)(uintptr_t)addr = (uint16_t)val; break;
    case 4: *(volatile uint32_t *)(uintptr_t)addr = (uint32_t)val; break;
    case 8: *(volatile uint64_t *)(uintptr_t)addr = val; break;
    default: con_puts("GROESSE MUSS 1, 2, 4 ODER 8 SEIN\n"); return;
    }
    con_puts("OK\n");
}

static void cmd_module(int argc, char **argv);   /* weiter unten: braucht err() */

static void cmd_pci(void) {
    for (int i = 0; i < pci_count(); i++) {
        const PciDev *d = pci_get(i);
        hexn(d->bus, 2);
        con_putc(':');
        hexn(d->dev, 2);
        con_putc('.');
        hexn(d->fn, 1);
        con_puts("  ");
        hexn(d->vendor, 4);
        con_putc(':');
        hexn(d->device, 4);
        con_puts("  ");
        hexn(d->cls, 2);
        con_putc('.');
        hexn(d->sub, 2);
        con_putc('.');
        hexn(d->prog, 2);
        con_puts("  ");
        con_puts(pci_class_name(d->cls, d->sub, d->prog));
        con_putc('\n');
    }
}

static void cmd_disks(void) {
    if (!blk_count()) {
        con_puts("KEINE BLOCKGERAETE\n");
        return;
    }
    con_puts("NAME        MIB     SEKTOR  TYP                 FS\n");
    for (int i = 0; i < blk_count(); i++) {
        BlockDev *d = blk_get(i);
        pad_puts(d->name, 10);
        pad_dec((d->sectors * d->sector_size) >> 20, 8);
        pad_dec(d->sector_size, 8);
        con_puts("  ");
        pad_puts(d->parent ? d->type : "LAUFWERK", 20);
        con_puts(d->fs[0] ? d->fs : "-");
        con_putc('\n');
    }
}

static void cmd_diskread(int argc, char **argv) {
    uint64_t lba;
    if (argc < 3 || !parse_num(argv[2], &lba)) {
        con_puts("DISKREAD <GERAET> <LBA>   (z.B. DISKREAD NVME0 0)\n");
        return;
    }
    BlockDev *d = blk_find(argv[1]);
    if (!d) {
        con_puts("UNBEKANNTES GERAET (DISKS)\n");
        return;
    }
    if (lba >= d->sectors) {
        con_puts("LBA AUSSERHALB DES GERAETS\n");
        return;
    }
    uint8_t *buf = kapi.malloc(d->sector_size);
    if (!buf) {
        con_puts("KEIN SPEICHER\n");
        return;
    }
    int r = d->read(d, lba, 1, buf);
    if (r) {
        con_puts("LESEFEHLER ");
        con_dec((uint64_t)(-r));
        con_puts("\n");
    } else {
        uint32_t n = d->sector_size < 256 ? d->sector_size : 256;
        for (uint32_t o = 0; o < n; o += 16) {
            hexn(o, 4);
            con_puts(": ");
            for (uint32_t i = 0; i < 16; i++) {
                hexn(buf[o + i], 2);
                con_putc(' ');
            }
            con_putc(' ');
            for (uint32_t i = 0; i < 16; i++) con_putc(buf[o + i] >= 32 && buf[o + i] < 127 ? (char)buf[o + i] : '.');
            con_putc('\n');
        }
    }
    kapi.free(buf);
}

/* ------------------------------------------------------------------ */
/* Dateien                                                             */
/* ------------------------------------------------------------------ */
static void err(int r) {
    con_puts("FEHLER: ");
    con_puts(exec_strerror(r));
    con_putc('\n');
}

static void print_path_bs(const char *p) {
    for (; *p; p++) con_putc(*p == '/' ? '\\' : *p);
}

/* ------------------------------------------------------------------ */
/* Programme und Module                                                */
/* ------------------------------------------------------------------ */
static void print_signed(int v) {
    if (v < 0) {
        con_putc('-');
        con_dec((uint64_t)(-(int64_t)v));
    } else {
        con_dec((uint64_t)v);
    }
}

/* RUN: in dieser Task ausfuehren und warten; START: eigene Task (Hintergrund) */
static void cmd_run(int argc, char **argv, int background) {
    if (argc < 2) {
        con_puts(background ? "START <DATEI> [ARGUMENTE]\n" : "RUN <DATEI> [ARGUMENTE]\n");
        return;
    }
    if (background) {
        int64_t r = exec_start(argv[1], argc - 1, argv + 1);
        if (r < 0) {
            err((int)r);
            return;
        }
        con_puts("TASK ");
        con_dec((uint64_t)r);
        con_puts(" GESTARTET\n");
    } else {
        int ret = 0;
        int r = exec_run(argv[1], argc - 1, argv + 1, &ret);
        if (r) {
            err(r);
            return;
        }
        if (ret) {
            con_puts("RUECKGABEWERT ");
            print_signed(ret);
            con_putc('\n');
        }
    }
}

static void cmd_module(int argc, char **argv) {
    if (argc >= 2 && streq_ci(argv[1], "list")) {
        con_puts("MODULE:\n");
        module_list();
    } else if (argc >= 3 && streq_ci(argv[1], "load")) {
        int r = module_load(argv[2]);
        if (r) err(r);
        else con_puts("OK\n");
    } else if (argc >= 3 && (streq_ci(argv[1], "remove") || streq_ci(argv[1], "unload"))) {
        int r = module_unload(argv[2]);
        if (r) err(r);
        else con_puts("OK\n");
    } else {
        con_puts("MODULE LIST | MODULE LOAD <DATEI.DRV> | MODULE REMOVE <NAME|NR>\n");
    }
}

/* ------------------------------------------------------------------ */
/* Befehle, die Module anmelden (kapi.cmd_register)                    */
/* ------------------------------------------------------------------ */
#define MAX_CMDS 32
#define CMD_NAME 16

static struct {
    char name[CMD_NAME];
    int (*fn)(int argc, char **argv);
} g_cmds[MAX_CMDS];

int shell_cmd_register(const char *name, int (*fn)(int argc, char **argv)) {
    if (!name || !name[0] || !fn) return FS_EINVAL;
    size_t n = strlen_(name);
    if (n >= CMD_NAME) return FS_ENAME;
    int slot = -1;
    for (int i = 0; i < MAX_CMDS; i++) {
        if (g_cmds[i].fn && streq_ci(g_cmds[i].name, name)) {   /* gleicher Name: ersetzen */
            slot = i;
            break;
        }
        if (!g_cmds[i].fn && slot < 0) slot = i;
    }
    if (slot < 0) return FS_ENOSPC;
    for (size_t k = 0; k <= n; k++) g_cmds[slot].name[k] = name[k];
    g_cmds[slot].fn = fn;
    return 0;
}

int shell_cmd_unregister(const char *name) {
    for (int i = 0; i < MAX_CMDS; i++) {
        if (g_cmds[i].fn && streq_ci(g_cmds[i].name, name)) {
            g_cmds[i].fn = 0;
            return 0;
        }
    }
    return FS_ENOENT;
}

void shell_cmds_remove_range(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < MAX_CMDS; i++) {
        uint64_t a = (uint64_t)(uintptr_t)g_cmds[i].fn;
        if (a && a >= lo && a < hi) g_cmds[i].fn = 0;
    }
}

typedef struct {
    int files, dirs;
    uint64_t bytes;
} DirStat;

static int dir_cb(const FsEntry *e, void *ctx) {
    DirStat *st = ctx;
    if (e->is_dir) {
        con_puts("  <DIR>       ");
        st->dirs++;
    } else {
        con_puts("  ");
        pad_dec(e->size, 10);
        con_puts("  ");
        st->files++;
        st->bytes += e->size;
    }
    con_puts(e->name);
    con_putc('\n');
    return 0;
}

static void cmd_dir(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    FsEntry e;
    int r = vfs_stat(path, &e);
    if (r) { err(r); return; }
    DirStat st = {0, 0, 0};
    if (!e.is_dir) {
        dir_cb(&e, &st);
        return;
    }
    r = vfs_readdir(path, dir_cb, &st);
    if (r) { err(r); return; }
    con_dec((uint64_t)st.files);
    con_puts(" DATEIEN, ");
    con_dec((uint64_t)st.dirs);
    con_puts(" VERZEICHNISSE, ");
    con_dec(st.bytes);
    con_puts(" BYTES\n");
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) {
        con_putc(vfs_cur_drive() ? vfs_cur_drive() : '?');
        con_puts(":");
        print_path_bs(vfs_cur_path());
        con_putc('\n');
        return;
    }
    int r = vfs_cd(argv[1]);
    if (r) err(r);
}

static void cmd_type(int argc, char **argv) {
    if (argc < 2) { con_puts("TYPE <DATEI>\n"); return; }
    uint8_t *buf;
    uint64_t len;
    int r = vfs_read_all(argv[1], &buf, &len);
    if (r) { err(r); return; }
    uint64_t shown = len > 8000 ? 8000 : len;
    for (uint64_t i = 0; i < shown; i++) {
        uint8_t c = buf[i];
        if (c == '\n') con_putc('\n');
        else if (c == '\r') continue;
        else if (c == '\t') con_puts("    ");
        else con_putc(c >= 32 && c < 127 ? (char)c : '.');
    }
    if (len > shown) con_puts("\n... (GEKUERZT)");
    if (len == 0 || buf[len - 1] != '\n') con_putc('\n');
    kapi.free(buf);
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    return b;
}

static void cmd_copy(int argc, char **argv) {
    if (argc < 3) { con_puts("COPY <QUELLE> <ZIEL>\n"); return; }
    uint8_t *buf;
    uint64_t len;
    int r = vfs_read_all(argv[1], &buf, &len);
    if (r) { err(r); return; }
    char dst[512];
    size_t n = 0;
    for (const char *p = argv[2]; *p && n < 250; p++) dst[n++] = *p;
    dst[n] = 0;
    FsEntry e;
    if (vfs_stat(dst, &e) == 0 && e.is_dir) {   /* Ziel ist ein Verzeichnis: Namen uebernehmen */
        dst[n++] = '/';
        for (const char *p = base_name(argv[1]); *p && n < 500; p++) dst[n++] = *p;
        dst[n] = 0;
    }
    r = vfs_write(dst, buf, len, 0);
    kapi.free(buf);
    if (r) err(r);
    else { con_dec(len); con_puts(" BYTES KOPIERT\n"); }
}

static void cmd_mount(int argc, char **argv) {
    if (argc == 1) {
        int any = 0;
        for (char l = 'A'; l <= 'Z'; l++) {
            Fs *fs = vfs_get(l);
            if (!fs) continue;
            any = 1;
            con_putc(l);
            con_puts(":  ");
            pad_puts(fs->dev ? fs->dev->name : "?", 10);
            pad_puts(fs->type, 7);
            pad_puts(fs->label, 13);
            con_puts(fs->readonly ? "NUR LESEN" : "LESEN+SCHREIBEN");
            con_putc('\n');
        }
        if (!any) con_puts("NICHTS EINGEBUNDEN (MOUNT <GERAET> <BUCHSTABE> [RW])\n");
        return;
    }
    if (argc < 3 || argv[2][1] != 0) { con_puts("MOUNT <GERAET> <BUCHSTABE> [RW]\n"); return; }
    int rw = argc > 3 && streq_ci(argv[3], "rw");
    int r = vfs_mount_dev(argv[1], argv[2][0], rw);
    if (r == FS_EEXIST && argc > 3) {   /* schon eingebunden: nur Schreibschutz umschalten */
        r = vfs_set_rw(argv[2][0], rw);
    }
    if (r) err(r);
    else {
        con_puts("OK, ");
        con_puts(rw ? "LESEN+SCHREIBEN\n" : "NUR LESEN\n");
    }
}

static void cmd_df(void) {
    uint64_t total, fr;
    int r = vfs_statfs(0, &total, &fr);
    if (r) { err(r); return; }
    con_dec(total >> 20);
    con_puts(" MIB GESAMT, ");
    con_dec(fr >> 20);
    con_puts(" MIB FREI\n");
}

/* Weiterleitung: Index des ">" oder ">>" (nicht in Anfuehrungszeichen) oder -1 */
static int find_redirect(int argc, char **argv, const uint8_t *quoted, int *append) {
    for (int i = 0; i < argc; i++) {
        if (quoted[i]) continue;
        if (streq_ci(argv[i], ">")) { *append = 0; return i; }
        if (streq_ci(argv[i], ">>")) { *append = 1; return i; }
    }
    return -1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* hexwrite 00 01 02 "Hallo" > datei    (>> haengt an)
 * Token ohne Anfuehrungszeichen: Hex-Bytes (z.B. 0A, 0xFF oder mehrere Bytes am Stueck: DEADBEEF).
 * Token in Anfuehrungszeichen: ASCII-Text. */
static void cmd_hexwrite(int argc, char **argv, const uint8_t *quoted) {
    int append = 0;
    int rd = find_redirect(argc, argv, quoted, &append);
    int end = rd >= 0 ? rd : argc;
    if (end < 2) { con_puts("HEXWRITE <BYTES/\"TEXT\" ...> [> DATEI]\n"); return; }

    size_t cap = 1;
    for (int i = 1; i < end; i++) cap += strlen_(argv[i]) + 1;
    uint8_t *buf = kapi.malloc(cap);
    if (!buf) { err(FS_ENOSPC); return; }
    size_t n = 0;
    for (int i = 1; i < end; i++) {
        const char *t = argv[i];
        if (quoted[i]) {
            while (*t) buf[n++] = (uint8_t)*t++;
            continue;
        }
        if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
        size_t l = strlen_(t);
        int ok = l > 0 && (l <= 2 || l % 2 == 0);
        for (size_t k = 0; ok && k < l; k++)
            if (hexval(t[k]) < 0) ok = 0;
        if (!ok) {
            con_puts("KEIN HEX-BYTE: ");
            con_puts(argv[i]);
            con_puts("  (TEXT IN \"ANFUEHRUNGSZEICHEN\")\n");
            kapi.free(buf);
            return;
        }
        if (l <= 2) {
            buf[n++] = (uint8_t)(l == 1 ? hexval(t[0]) : hexval(t[0]) * 16 + hexval(t[1]));
        } else {
            for (size_t k = 0; k < l; k += 2) buf[n++] = (uint8_t)(hexval(t[k]) * 16 + hexval(t[k + 1]));
        }
    }
    if (rd >= 0) {
        if (rd + 2 != argc) { con_puts("NACH > FEHLT GENAU EIN DATEINAME\n"); kapi.free(buf); return; }
        int r = vfs_write(argv[rd + 1], buf, n, append);
        if (r) err(r);
        else {
            con_dec(n);
            con_puts(" BYTES GESCHRIEBEN\n");
        }
    } else {
        con_dec(n);
        con_puts(" BYTES:");
        for (size_t i = 0; i < n && i < 64; i++) {
            con_putc(' ');
            hexn(buf[i], 2);
        }
        con_putc('\n');
    }
    kapi.free(buf);
}

static void do_reboot(void) {
    con_puts("NEUSTART...\n");
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {
    }
    outb(0x64, 0xFE);   /* Reset ueber den Tastaturcontroller */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {0, 0};
    __asm__ volatile("cli\n\tlidt %0\n\tint3" : : "m"(idtr));   /* Fallback: Triple Fault */
    kernel_halt();
}

static void do_shutdown(void) {
    outw(0x604, 0x2000);    /* QEMU (neuere Versionen) */
    outw(0xB004, 0x2000);   /* Bochs / aeltere QEMU */
    outw(0x4004, 0x3400);   /* VirtualBox */
    con_puts("AUSSCHALTEN GEHT BISHER NUR IN EINER VM (ECHTE HARDWARE BRAUCHT ACPI, FOLGT)\n");
}

static void cmd_echo(int argc, char **argv, const uint8_t *quoted) {
    int append = 0;
    int rd = find_redirect(argc, argv, quoted, &append);
    int end = rd >= 0 ? rd : argc;
    if (rd < 0) {
        for (int i = 1; i < end; i++) {
            con_puts(argv[i]);
            con_putc(i + 1 < end ? ' ' : '\n');
        }
        if (end == 1) con_putc('\n');
        return;
    }
    if (rd + 2 != argc) { con_puts("NACH > FEHLT GENAU EIN DATEINAME\n"); return; }
    size_t cap = 2;
    for (int i = 1; i < end; i++) cap += strlen_(argv[i]) + 1;
    char *buf = kapi.malloc(cap);
    if (!buf) { err(FS_ENOSPC); return; }
    size_t n = 0;
    for (int i = 1; i < end; i++) {
        for (const char *p = argv[i]; *p; p++) buf[n++] = *p;
        if (i + 1 < end) buf[n++] = ' ';
    }
    buf[n++] = '\n';
    int r = vfs_write(argv[rd + 1], buf, n, append);
    kapi.free(buf);
    if (r) err(r);
}

static void run_line(char *line) {
    char *argv[ARG_MAX];
    uint8_t quoted[ARG_MAX];
    int argc = 0;
    char *s = line;
    while (*s && argc < ARG_MAX) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"') {   /* "Text mit Leerzeichen" */
            s++;
            argv[argc] = s;
            quoted[argc++] = 1;
            while (*s && *s != '"') s++;
            if (*s) *s++ = 0;
        } else {
            argv[argc] = s;
            quoted[argc++] = 0;
            while (*s && *s != ' ' && *s != '\t') s++;
            if (*s) *s++ = 0;
        }
    }
    if (!argc) return;

    const char *c = argv[0];
    if (c[0] && c[1] == ':' && !c[2]) {   /* Laufwerkswechsel: E: */
        int r = vfs_set_drive(c[0]);
        if (r) err(r);
        return;
    }
    for (int i = 0; i < MAX_CMDS; i++) {   /* von Modulen angemeldete Befehle haben Vorrang */
        if (g_cmds[i].fn && streq_ci(g_cmds[i].name, c)) {
            g_cmds[i].fn(argc, argv);
            return;
        }
    }
    if (streq_ci(c, "help") || streq_ci(c, "?")) cmd_help();
    else if (streq_ci(c, "clear") || streq_ci(c, "cls")) con_reset();
    else if (streq_ci(c, "echo")) cmd_echo(argc, argv, quoted);
    else if (streq_ci(c, "ver")) {
        con_puts("BIYOS TEST-KERNEL, KAPI VERSION ");
        con_dec(kapi.version);
        con_puts("\n");
    } else if (streq_ci(c, "mem")) cmd_mem();
    else if (streq_ci(c, "uptime")) cmd_uptime();
    else if (streq_ci(c, "tasks")) sched_list();
    else if (streq_ci(c, "module")) cmd_module(argc, argv);
    else if (streq_ci(c, "run")) cmd_run(argc, argv, 0);
    else if (streq_ci(c, "start")) cmd_run(argc, argv, 1);
    else if (streq_ci(c, "layout")) {
        if (argc > 1 && streq_ci(argv[1], "de")) kbd_set_layout(1);
        else if (argc > 1 && streq_ci(argv[1], "us")) kbd_set_layout(0);
        con_puts("LAYOUT: ");
        con_puts(kbd_layout_de() ? "DE\n" : "US\n");
    } else if (streq_ci(c, "peek")) cmd_peek(argc, argv);
    else if (streq_ci(c, "poke")) cmd_poke(argc, argv);
    else if (streq_ci(c, "pci")) cmd_pci();
    else if (streq_ci(c, "disks")) cmd_disks();
    else if (streq_ci(c, "diskread")) cmd_diskread(argc, argv);
    else if (streq_ci(c, "mount")) cmd_mount(argc, argv);
    else if (streq_ci(c, "umount")) {
        int r = argc > 1 ? vfs_unmount(argv[1][0]) : FS_EINVAL;
        if (r) err(r);
    } else if (streq_ci(c, "dir") || streq_ci(c, "ls")) cmd_dir(argc, argv);
    else if (streq_ci(c, "cd") || streq_ci(c, "pwd")) cmd_cd(streq_ci(c, "pwd") ? 1 : argc, argv);
    else if (streq_ci(c, "type") || streq_ci(c, "cat")) cmd_type(argc, argv);
    else if (streq_ci(c, "mkdir") || streq_ci(c, "md")) {
        int r = argc > 1 ? vfs_mkdir(argv[1]) : FS_EINVAL;
        if (r) err(r);
    } else if (streq_ci(c, "del") || streq_ci(c, "rm") || streq_ci(c, "rmdir")) {
        int r = argc > 1 ? vfs_remove(argv[1]) : FS_EINVAL;
        if (r) err(r);
    } else if (streq_ci(c, "copy") || streq_ci(c, "cp")) cmd_copy(argc, argv);
    else if (streq_ci(c, "ren") || streq_ci(c, "mv")) {
        int r = argc > 2 ? vfs_rename(argv[1], argv[2]) : FS_EINVAL;
        if (r) err(r);
    } else if (streq_ci(c, "df")) cmd_df();
    else if (streq_ci(c, "hexwrite")) cmd_hexwrite(argc, argv, quoted);
    else if (streq_ci(c, "reboot")) do_reboot();
    else if (streq_ci(c, "shutdown")) do_shutdown();
    else {
        FsEntry fe;   /* kein Befehl: ist es eine Datei? Dann als Programm ausfuehren (wie bei DOS) */
        if (vfs_stat(c, &fe) == 0 && !fe.is_dir) {
            char *av[ARG_MAX + 1];   /* "run" + die getippte Zeile (Dateiname und Argumente) */
            av[0] = (char *)"run";
            for (int i = 0; i < argc; i++) av[i + 1] = argv[i];
            cmd_run(argc + 1, av, 0);
            return;
        }
        con_puts("UNBEKANNTER BEFEHL: ");
        con_puts(c);
        con_puts("  (HELP)\n");
    }
}

/* ------------------------------------------------------------------ */
/* Zeileneingabe                                                       */
/* ------------------------------------------------------------------ */
static void readline(char *line, int max) {
    int n = 0;
    for (;;) {
        con_putc('_');   /* Cursor */
        int c;
        while ((c = kbd_getc()) < 0) kapi.yield();
        con_backspace();
        if (c == '\n') {
            con_putc('\n');
            line[n] = 0;
            return;
        }
        if (c == 3) {   /* Ctrl+C: Zeile verwerfen */
            con_puts("^C\n");
            line[0] = 0;
            return;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                con_backspace();
            }
            continue;
        }
        if (c >= 32 && c < 127 && n < max - 1) {
            line[n++] = (char)c;
            con_putc((char)c);
        }
    }
}

void shell_main(void *arg) {
    (void)arg;
    char line[LINE_MAX];
    con_color(120, 255, 160);
    con_puts("\nBIYOS SHELL - HELP FUER BEFEHLE\n");
    for (;;) {
        con_color(120, 200, 255);
        con_puts("BIYOS");
        if (vfs_cur_drive()) {
            con_putc(' ');
            con_putc(vfs_cur_drive());
            con_putc(':');
            print_path_bs(vfs_cur_path());
        }
        con_puts("> ");
        con_color(230, 230, 230);
        readline(line, LINE_MAX);
        run_line(line);
    }
}
