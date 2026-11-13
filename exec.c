/* SPDX-License-Identifier: MIT */
/*
 * exec.c - Executable-Loader, Kernel-Module und IMPORT-Abschnitt
 *
 * ---------------------------------------------------------------------------
 * Dateiformat eines BIYOS-Executables (alle Mehrbyte-Zahlen big-endian, damit sie
 * im Hex-Dump und beim Schreiben per hexwrite lesbar bleiben):
 *
 *   Byte  0       0xAA
 *   Byte  1..3    "EXE"
 *   Byte  4..7    Typ: "DRIV" (Treiber/Kernel-Modul), "STRT" (Startprogramm), "PLSE" (Programm)
 *   Byte  8..11   Version (Beispiel: 00 00 00 01)
 *   Byte 12..19   8 Bytes, werden uebersprungen
 *   [optional, ab Byte 20]  IMPORT-Abschnitt:
 *       "IMPORT"            6 Bytes ASCII
 *       Laenge              4 Bytes = Laenge der Eintraege in Bytes (ohne diese 10 Bytes)
 *       Eintraege, je:
 *           X       4 Bytes  Start in der Quelldatei (Byte-Offset, ab 0)
 *           Y       4 Bytes  Ende in der Quelldatei (exklusiv): die Funktion sind die Bytes X .. Y-1
 *           ZIEL    4 Bytes  Offset IM EXECUTABLE (ab Byte 0 der Datei), wohin die Bytes kopiert werden
 *           NL      1 Byte   Laenge des Dateinamens
 *           NAME    NL Bytes ASCII-Dateiname der Quelldatei
 *   danach der Code. Der Einsprung ist immer das erste Byte nach Header (und IMPORT-Abschnitt).
 *
 * Die ganze Datei wird an eine 16-Byte-ausgerichtete Heap-Adresse geladen und dort ausgefuehrt.
 * Deshalb muss der Code positionsunabhaengig sein (rip-relative Adressen, kein absoluter Zugriff
 * auf die eigenen Bytes).
 *
 * IMPORT ohne GOT/PLT: Das Executable reserviert an der Stelle ZIEL einen freien Platz von
 * mindestens Y-X Bytes (z.B. mit NOPs oder Nullen). Der Loader kopiert die Bytes X..Y-1 der
 * Quelldatei dorthin, BEVOR das Programm startet. Das Programm ruft die Funktion dann mit
 * einem normalen "call rel32" auf diesen Platz. Die importierten Bytes muessen selbst
 * positionsunabhaengig sein und sich an die System-V-ABI halten.
 * Einen Dateinamen ohne Pfad sucht der Loader zuerst im Ordner des Executables, dann im
 * aktuellen Ordner.
 *
 * ---------------------------------------------------------------------------
 * Aufruf des Einsprungs (System-V-ABI):
 *
 *     int entry(KernelApi *api, int argc, char **argv);
 *
 * PLSE/STRT: argc/argv wie bei einem normalen Programm (argv[0] = Pfad wie getippt).
 * DRIV:      beim Laden argc = 0, argv = NULL.
 *            beim Entladen (module remove) argc = -1. Rueckgabe != 0 heisst "bleibe geladen".
 * Der Rueckgabewert 0 heisst "ok". Der Loader setzt eax vor dem Aufruf auf 0, ein Programm,
 * das nur aus einem "ret" (0xC3) besteht, ist also ein gueltiges Programm. Die vom System-V-ABI
 * geforderten Register (rbx, rbp, r12-r15) rettet der Loader selbst.
 *
 * ---------------------------------------------------------------------------
 * Lebensdauer:
 *   run    - das Programm laeuft in der aufrufenden Task (wie ein Funktionsaufruf). Danach wird sein
 *            Speicher freigegeben. Es darf also keine Tasks, Interrupt-Handler oder Befehle
 *            zuruecklassen, die auf seinen Code zeigen (dafuer gibt es start).
 *   start  - das Programm bekommt eine eigene Task. Kehrt es zurueck, wird sein Speicher freigegeben.
 *            Ruft es stattdessen kapi.task_exit() auf, bleibt der Speicher belegt (resident).
 *   module - ein DRIV-Executable wird dauerhaft geladen. Befehle, die es per kapi.cmd_register
 *            anmeldet, werden beim Entladen automatisch entfernt (Zuordnung ueber die Adresse
 *            der Funktion). Alles andere (Interrupt-Handler, Tasks, ersetzte kapi-Eintraege)
 *            muss das Modul beim Entladen (argc == -1) selbst wieder aufraeumen.
 *   Module des Bootloaders liegen im Loader-Speicher und werden beim Entladen nicht freigegeben.
 */
#include "kernel.h"

void *memcpy(void *d, const void *s, size_t n);

#define HDR_SIZE    20
#define IMPORT_HDR  10    /* "IMPORT" + 4 Byte Laenge */
#define MAX_MODULES 32

typedef struct {
    uint8_t *base;    /* geladene Datei (Heap oder Loader-Speicher) */
    uint64_t size;
    uint64_t entry;   /* Offset des Einsprungs */
    uint32_t version;
    char     type[5];
} Image;

typedef struct {
    int      used, boot;
    char     name[32];
    char     type[5];
    uint32_t version;
    uint8_t *base;
    uint64_t size;
    uint64_t entry;
} Module;

static Module g_mod[MAX_MODULES];

/* ------------------------------------------------------------------ */
/* Aufruf in Maschinencode (Register retten, eax = 0, Ausrichtung)     */
/* ------------------------------------------------------------------ */
int exec_call(void *entry, KernelApi *api, int argc, char **argv);
__asm__(
    ".text\n"
    ".globl exec_call\n"
    ".type exec_call,@function\n"
    "exec_call:\n"
    "    pushq %rbx\n"
    "    pushq %rbp\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq  $8, %rsp\n"        /* 6 Pushes + Rueckadresse = 56 Bytes: noch 8, damit rsp % 16 == 0 */
    "    movq  %rdi, %r11\n"      /* Einsprung */
    "    movq  %rsi, %rdi\n"      /* api  */
    "    movq  %rdx, %rsi\n"      /* argc */
    "    movq  %rcx, %rdx\n"      /* argv */
    "    xorl  %eax, %eax\n"
    "    cld\n"
    "    callq *%r11\n"
    "    addq  $8, %rsp\n"
    "    popq  %r15\n"
    "    popq  %r14\n"
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbp\n"
    "    popq  %rbx\n"
    "    retq\n"
);

/* ------------------------------------------------------------------ */
/* Kleine Helfer                                                       */
/* ------------------------------------------------------------------ */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int is_type(const char *t, const char *want) {
    return t[0] == want[0] && t[1] == want[1] && t[2] == want[2] && t[3] == want[3];
}

static char lc(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int streq_ci(const char *a, const char *b) {
    while (*a && lc(*a) == lc(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

static int is_sep(char c) {
    return c == '/' || c == '\\' || c == ':';
}

static int has_sep(const char *s) {
    for (; *s; s++)
        if (is_sep(*s)) return 1;
    return 0;
}

/* Ordner-Teil eines Pfades inklusive letztem Trenner ("E:\\dir\\x.drv" -> "E:\\dir\\") */
static void dir_of(const char *path, char *out, size_t max) {
    size_t n = slen(path), cut = 0;
    for (size_t i = 0; i < n; i++)
        if (is_sep(path[i])) cut = i + 1;
    if (cut >= max) cut = 0;
    for (size_t i = 0; i < cut; i++) out[i] = path[i];
    out[cut] = 0;
}

static void base_name(const char *path, char *out, size_t max) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (is_sep(*p)) b = p + 1;
    size_t i = 0;
    for (; b[i] && i + 1 < max; i++) out[i] = b[i];
    out[i] = 0;
}

const char *exec_strerror(int err) {
    switch (err) {
    case EX_EHEADER: return "KEIN GUELTIGER BIYOS-HEADER (0xAA \"EXE\" TYP ...)";
    case EX_ETYPE:   return "FALSCHER TYP FUER DIESEN BEFEHL";
    case EX_EIMPORT: return "IMPORT-ABSCHNITT UNGUELTIG ODER QUELLE NICHT LESBAR";
    case EX_ENOCODE: return "KEIN CODE NACH DEM HEADER";
    case EX_EINIT:   return "MODUL-INIT HAT FEHLER GEMELDET";
    case EX_EBUSY:   return "MODUL LEHNT DAS ENTLADEN AB";
    case EX_ELIMIT:  return "ZU VIELE MODULE";
    case EX_ENOMOD:  return "MODUL NICHT GELADEN";
    default:         return fs_strerror(err);
    }
}

/* ------------------------------------------------------------------ */
/* IMPORT: Funktionsbytes aus anderen Dateien in den Platz kopieren    */
/* ------------------------------------------------------------------ */
static int apply_imports(uint8_t *base, uint64_t size, const uint8_t *p, uint32_t len, uint64_t code,
                         const char *dir) {
    const uint8_t *end = p + len;
    while (p < end) {
        if ((uint64_t)(end - p) < 13) return EX_EIMPORT;
        uint32_t x = be32(p), y = be32(p + 4), dest = be32(p + 8), nl = p[12];
        p += 13;
        if (nl == 0 || nl > 120 || (uint64_t)(end - p) < nl) return EX_EIMPORT;
        char name[128];
        memcpy(name, p, nl);
        name[nl] = 0;
        p += nl;
        if (y <= x) return EX_EIMPORT;
        uint64_t n = (uint64_t)y - x;
        if (dest < code || (uint64_t)dest + n > size) return EX_EIMPORT;   /* Platz muss im Code-Bereich liegen */

        int ok = 0;
        if (dir[0] && !has_sep(name) && slen(dir) + nl < sizeof(name) * 2) {
            char path[256];
            size_t k = 0;
            for (const char *d = dir; *d; d++) path[k++] = *d;
            for (const char *d = name; *d; d++) path[k++] = *d;
            path[k] = 0;
            ok = vfs_read(path, x, base + dest, n) == (int64_t)n;
        }
        if (!ok) ok = vfs_read(name, x, base + dest, n) == (int64_t)n;
        if (!ok) {
            con_puts("IMPORT: ");
            con_puts(name);
            con_puts(" BYTES ");
            con_dec(x);
            con_puts("-");
            con_dec(y);
            con_puts(" NICHT LESBAR\n");
            return EX_EIMPORT;
        }
    }
    return 0;
}

/* Header pruefen, IMPORTs einsetzen, Einsprung bestimmen */
static int image_prepare(uint8_t *base, uint64_t size, const char *dir, Image *img) {
    if (size < HDR_SIZE || base[0] != 0xAA || base[1] != 'E' || base[2] != 'X' || base[3] != 'E') return EX_EHEADER;
    for (int k = 0; k < 4; k++) img->type[k] = (char)base[4 + k];
    img->type[4] = 0;
    if (!is_type(img->type, "DRIV") && !is_type(img->type, "STRT") && !is_type(img->type, "PLSE")) return EX_EHEADER;
    img->version = be32(base + 8);

    uint64_t code = HDR_SIZE;
    if (size >= HDR_SIZE + IMPORT_HDR && base[20] == 'I' && base[21] == 'M' && base[22] == 'P' && base[23] == 'O' &&
        base[24] == 'R' && base[25] == 'T') {
        uint32_t len = be32(base + 26);
        if ((uint64_t)len > size - HDR_SIZE - IMPORT_HDR) return EX_EIMPORT;
        code = HDR_SIZE + IMPORT_HDR + len;
        int r = apply_imports(base, size, base + HDR_SIZE + IMPORT_HDR, len, code, dir);
        if (r) return r;
    }
    if (code >= size) return EX_ENOCODE;
    img->base = base;
    img->size = size;
    img->entry = code;
    return 0;
}

static int image_from_file(const char *path, Image *img) {
    uint8_t *buf;
    uint64_t len;
    int r = vfs_read_all(path, &buf, &len);
    if (r) return r;
    char dir[256];
    dir_of(path, dir, sizeof(dir));
    r = image_prepare(buf, len, dir, img);
    if (r) kapi.free(buf);
    return r;
}

/* ------------------------------------------------------------------ */
/* Programme: run (in der aufrufenden Task) und start (eigene Task)    */
/* ------------------------------------------------------------------ */
int exec_run(const char *path, int argc, char **argv, int *ret) {
    Image img;
    int r = image_from_file(path, &img);
    if (r) return r;
    if (!is_type(img.type, "PLSE") && !is_type(img.type, "STRT")) {
        kapi.free(img.base);
        return EX_ETYPE;
    }
    int rv = exec_call(img.base + img.entry, &kapi, argc, argv);
    kapi.free(img.base);
    if (ret) *ret = rv;
    return 0;
}

typedef struct {
    uint8_t *base;
    uint64_t entry;
    int      argc;
    char   **argv;
} StartCtx;

static void start_main(void *p) {
    StartCtx *c = p;
    uint8_t *base = c->base;
    exec_call(base + c->entry, &kapi, c->argc, c->argv);
    kapi.free(base);   /* nur wenn das Programm zurueckkehrt; bei task_exit() bleibt es resident */
    kapi.free(c);
    kapi.task_exit();
}

int64_t exec_start(const char *path, int argc, char **argv) {
    Image img;
    int r = image_from_file(path, &img);
    if (r) return r;
    if (!is_type(img.type, "PLSE") && !is_type(img.type, "STRT")) {
        kapi.free(img.base);
        return EX_ETYPE;
    }
    /* Argumente kopieren: die Kommandozeile der Shell lebt nur bis zum naechsten Befehl */
    size_t total = ((size_t)argc + 1) * sizeof(char *);
    for (int i = 0; i < argc; i++) total += slen(argv[i]) + 1;
    StartCtx *c = kapi.malloc(sizeof(StartCtx) + total);
    if (!c) {
        kapi.free(img.base);
        return FS_ENOSPC;
    }
    char **av = (char **)(c + 1);
    char *s = (char *)(av + argc + 1);
    for (int i = 0; i < argc; i++) {
        av[i] = s;
        for (const char *a = argv[i]; *a; a++) *s++ = *a;
        *s++ = 0;
    }
    av[argc] = 0;
    c->base = img.base;
    c->entry = img.entry;
    c->argc = argc;
    c->argv = av;

    char name[24];
    base_name(path, name, sizeof(name));
    uint64_t id = kapi.task_create(name, start_main, c);
    if (!id) {
        kapi.free(img.base);
        kapi.free(c);
        return FS_ENOSPC;
    }
    return (int64_t)id;
}

/* ------------------------------------------------------------------ */
/* Kernel-Module (DRIV)                                                */
/* ------------------------------------------------------------------ */
static int module_find(const char *name) {
    for (int i = 0; i < MAX_MODULES; i++)
        if (g_mod[i].used && streq_ci(g_mod[i].name, name)) return i;
    return -1;
}

/* Modul eintragen und seinen Einsprung mit argc = 0 aufrufen. Bei Fehler wieder austragen. */
static int module_start(const Image *img, const char *name, int boot) {
    if (!is_type(img->type, "DRIV")) return EX_ETYPE;
    int slot = -1;
    for (int i = 0; i < MAX_MODULES; i++)
        if (!g_mod[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) return EX_ELIMIT;
    Module *m = &g_mod[slot];
    size_t k = 0;
    for (; name[k] && k + 1 < sizeof(m->name); k++) m->name[k] = name[k];
    m->name[k] = 0;
    for (int i = 0; i < 5; i++) m->type[i] = img->type[i];
    m->version = img->version;
    m->base = img->base;
    m->size = img->size;
    m->entry = img->entry;
    m->boot = boot;
    m->used = 1;

    int rv = exec_call(m->base + m->entry, &kapi, 0, 0);
    if (rv != 0) {
        shell_cmds_remove_range((uint64_t)(uintptr_t)m->base, (uint64_t)(uintptr_t)m->base + m->size);
        m->used = 0;
        con_puts("MODUL ");
        con_puts(name);
        con_puts(" MELDET FEHLER ");
        con_dec((uint32_t)rv);
        con_putc('\n');
        return EX_EINIT;
    }
    return 0;
}

int module_load(const char *path) {
    char name[32];
    base_name(path, name, sizeof(name));
    if (!name[0]) return FS_EINVAL;
    if (module_find(name) >= 0) return FS_EEXIST;
    Image img;
    int r = image_from_file(path, &img);
    if (r) return r;
    r = module_start(&img, name, 0);
    if (r) kapi.free(img.base);
    return r;
}

int module_unload(const char *which) {
    int idx = -1;
    int num = 1, v = 0;
    for (const char *p = which; *p; p++) {
        if (*p < '0' || *p > '9') {
            num = 0;
            break;
        }
        v = v * 10 + (*p - '0');
    }
    if (num && which[0]) {
        if (v >= 1 && v <= MAX_MODULES && g_mod[v - 1].used) idx = v - 1;
    } else {
        idx = module_find(which);
    }
    if (idx < 0) return EX_ENOMOD;
    Module *m = &g_mod[idx];
    if (exec_call(m->base + m->entry, &kapi, -1, 0) != 0) return EX_EBUSY;
    shell_cmds_remove_range((uint64_t)(uintptr_t)m->base, (uint64_t)(uintptr_t)m->base + m->size);
    if (!m->boot) kapi.free(m->base);
    m->used = 0;
    return 0;
}

void module_list(void) {
    int n = 0;
    for (int i = 0; i < MAX_MODULES; i++) {
        const Module *m = &g_mod[i];
        if (!m->used) continue;
        n++;
        con_puts("  ");
        con_dec((uint64_t)i + 1);
        con_puts("  ");
        con_puts(m->name);
        con_puts("  ");
        con_puts(m->type);
        con_puts("  V");
        con_dec(m->version);
        con_puts("  ");
        con_dec(m->size);
        con_puts(" BYTES AT ");
        con_hex((uint64_t)(uintptr_t)m->base);
        if (m->boot) con_puts("  (BOOTLOADER)");
        con_putc('\n');
    }
    if (!n) con_puts("  KEINE MODULE GELADEN\n");
}

/* Die vom Bootloader gelesenen Module starten (nach dem Einbinden der Laufwerke, damit IMPORT
 * auch Dateien von dort lesen kann). */
void module_load_boot(const BootInfo *bi) {
    const BootModule *bm = (const BootModule *)(uintptr_t)bi->modules;
    for (uint64_t i = 0; i < bi->module_count; i++) {
        Image img;
        con_puts("MODUL ");
        con_puts(bm[i].name);
        con_puts(": ");
        int r = image_prepare((uint8_t *)(uintptr_t)bm[i].addr, bm[i].size, "", &img);
        if (r == 0) r = module_start(&img, bm[i].name, 1);
        if (r == 0) con_puts("OK\n");
        else {
            con_puts("UEBERSPRUNGEN - ");
            con_puts(exec_strerror(r));
            con_putc('\n');
        }
    }
}

/* ------------------------------------------------------------------ */
/* Start-Ordner: jedes Executable mit Typ STRT wird beim Boot gestartet */
/* ------------------------------------------------------------------ */
#ifndef BIYOS_START_DRIVE
#define BIYOS_START_DRIVE 'C'   /* nur fuer Tests anders setzen, z.B. -DBIYOS_START_DRIVE=69 fuer E: */
#endif
#define START_MAX  64
#define START_NAME 100

typedef struct {
    char names[START_MAX][START_NAME];
    int  count, skipped;
} StartList;

static int start_collect(const FsEntry *fe, void *ctx) {
    StartList *l = ctx;
    if (fe->is_dir) return 0;
    if (slen(fe->name) >= START_NAME) {   /* zu langer Name: nicht starten, aber melden */
        l->skipped++;
        return 0;
    }
    if (l->count >= START_MAX) {
        l->skipped++;
        return 0;
    }
    char *d = l->names[l->count++];
    size_t i = 0;
    for (; fe->name[i]; i++) d[i] = fe->name[i];
    d[i] = 0;
    return 0;
}

static int name_less(const char *a, const char *b) {
    while (*a && lc(*a) == lc(*b)) {
        a++;
        b++;
    }
    return lc(*a) < lc(*b);
}

int start_run_all(const BootInfo *bi) {
    char dirz[9] = {BIYOS_START_DRIVE, ':', '/', 'S', 't', 'a', 'r', 't', 0};

    if (bi->boot_mode == BOOT_MODE_NO_MODULES) {   /* Rettungsweg: "ohne Module" laesst den Start-Ordner aus */
        con_puts("START: UEBERSPRUNGEN (OHNE MODULE GEBOOTET)\n");
        return 0;
    }
    StartList *l = kapi.malloc(sizeof(StartList));
    if (!l) return 0;
    l->count = l->skipped = 0;
    if (vfs_readdir(dirz, start_collect, l) != 0) {   /* Laufwerk oder Ordner fehlt: nichts zu tun */
        kapi.free(l);
        return 0;
    }
    for (int i = 1; i < l->count; i++) {   /* nach Namen sortieren, damit die Reihenfolge feststeht */
        char tmp[START_NAME];
        int j = i;
        for (size_t k = 0; k < START_NAME; k++) tmp[k] = l->names[i][k];
        while (j > 0 && name_less(tmp, l->names[j - 1])) {
            for (size_t k = 0; k < START_NAME; k++) l->names[j][k] = l->names[j - 1][k];
            j--;
        }
        for (size_t k = 0; k < START_NAME; k++) l->names[j][k] = tmp[k];
    }

    int started = 0;
    for (int i = 0; i < l->count; i++) {
        char path[128];
        size_t n = 0;
        for (const char *p = dirz; *p; p++) path[n++] = *p;
        path[n++] = '/';
        for (const char *p = l->names[i]; *p; p++) path[n++] = *p;
        path[n] = 0;

        uint8_t h[HDR_SIZE];   /* nur der Header entscheidet: Typ STRT, sonst ignorieren */
        if (vfs_read(path, 0, h, HDR_SIZE) != HDR_SIZE) continue;
        if (h[0] != 0xAA || h[1] != 'E' || h[2] != 'X' || h[3] != 'E') continue;
        if (!(h[4] == 'S' && h[5] == 'T' && h[6] == 'R' && h[7] == 'T')) continue;

        char *argv[1] = {path};
        int64_t r = exec_start(path, 1, argv);
        con_puts("START: ");
        con_puts(l->names[i]);
        if (r < 0) {
            con_puts(" FEHLGESCHLAGEN: ");
            con_puts(exec_strerror((int)r));
        } else {
            con_puts(" (TASK ");
            con_dec((uint64_t)r);
            con_putc(')');
            started++;
        }
        con_putc('\n');
    }
    if (l->skipped) {
        con_puts("START: ");
        con_dec((uint64_t)l->skipped);
        con_puts(" DATEIEN UEBERSPRUNGEN (NAME ZU LANG ODER MEHR ALS 64)\n");
    }
    kapi.free(l);
    return started;
}
