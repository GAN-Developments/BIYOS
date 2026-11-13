/* SPDX-License-Identifier: MIT */
/*
 * fat32.c - FAT32-Dateisystem (Lesen und Schreiben, lange Dateinamen, Unterverzeichnisse)
 *
 * Arbeitet auf einem BlockDev (Partition). Pfade kommen normalisiert von vfs.c ("/dir/datei").
 *   - lange Namen (LFN) werden gelesen und beim Anlegen erzeugt (nur ASCII), samt 8.3-Alias
 *   - Namensvergleich ohne Beachtung der Gross-/Kleinschreibung
 *   - FAT-Aenderungen gehen sofort in alle FAT-Kopien, FSInfo wird nach jeder Aenderung gesichert
 *   - Dateien bis 4 GiB - 1 (FAT32-Grenze)
 * Nicht enthalten: Unicode jenseits von ASCII, Attribute setzen, Zeitstempel erhalten beim
 * Umbenennen, Defragmentierung. Der Schreibschutz (Fs.readonly) wird in vfs.c geprueft.
 */
#include "kernel.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#define EOC_MIN   0x0FFFFFF8u
#define EOC       0x0FFFFFFFu
#define BAD_CL    0x0FFFFFF7u

#define ATTR_VOL  0x08
#define ATTR_DIR  0x10
#define ATTR_ARC  0x20
#define ATTR_LFN  0x0F

typedef struct {
    uint32_t cluster;
    uint32_t index;   /* Eintragsnummer innerhalb des Clusters */
} DirPos;

typedef struct {
    BlockDev *dev;
    uint32_t ss, spc, cbytes, epc;          /* Sektorgroesse, Sektoren/Cluster, Bytes/Cluster, Eintraege/Cluster */
    uint64_t fat_lba, data_lba;
    uint32_t fat_secs, nfats, root, nclusters;
    uint32_t fsinfo;                        /* Sektor der FSInfo, 0 = keine */
    uint32_t free_count, next_free;
    uint8_t *fatbuf; uint32_t fatsec; int fat_ok;
    uint8_t *secbuf; uint64_t seclba; int sec_ok;
    uint8_t *clbuf;
} Fat32;

typedef struct {
    uint8_t e[32];        /* Kurzeintrag */
    DirPos spos;          /* Position des Kurzeintrags */
    DirPos lpos;          /* Position des ersten Eintrags der Gruppe (LFN oder Kurzeintrag) */
    uint32_t nent;        /* Eintraege der Gruppe */
    uint32_t dir;         /* Verzeichnis-Cluster */
    char name[256];
} Found;

static inline uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void p32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int streq_ci(const char *a, const char *b) {
    while (*a && lc(*a) == lc(*b)) { a++; b++; }
    return *a == *b;
}
static void scopy(char *d, const char *s, size_t max) {
    size_t i = 0;
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* ------------------------------------------------------------------ */
/* Zeit                                                                */
/* ------------------------------------------------------------------ */
static uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }

static void fat_now(uint16_t *date, uint16_t *time) {
    for (int i = 0; i < 1000 && (cmos(0x0A) & 0x80); i++) {
    }
    uint32_t s = cmos(0), m = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9), b = cmos(0x0B);
    if (!(b & 4)) {   /* BCD */
        s = (s & 15) + (s >> 4) * 10; m = (m & 15) + (m >> 4) * 10; h = (h & 15) + ((h >> 4) & 7) * 10;
        d = (d & 15) + (d >> 4) * 10; mo = (mo & 15) + (mo >> 4) * 10; y = (y & 15) + (y >> 4) * 10;
    }
    uint32_t year = 2000 + y;
    if (year < 1980 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || m > 59 || s > 59) {
        year = 2026; mo = 1; d = 1; h = 0; m = 0; s = 0;
    }
    *date = (uint16_t)(((year - 1980) << 9) | (mo << 5) | d);
    *time = (uint16_t)((h << 11) | (m << 5) | (s / 2));
}

/* ------------------------------------------------------------------ */
/* Geraetezugriff, FAT                                                 */
/* ------------------------------------------------------------------ */
static int rd(Fat32 *f, uint64_t lba, uint32_t n, void *buf) {
    return f->dev->read(f->dev, lba, n, buf) ? FS_EIO : 0;
}

static int wr(Fat32 *f, uint64_t lba, uint32_t n, const void *buf) {
    if (f->sec_ok && f->seclba >= lba && f->seclba < lba + n) f->sec_ok = 0;
    return f->dev->write(f->dev, lba, n, buf) ? FS_EIO : 0;
}

static int valid(Fat32 *f, uint32_t c) { return c >= 2 && c <= f->nclusters + 1; }
static uint64_t clba(Fat32 *f, uint32_t c) { return f->data_lba + (uint64_t)(c - 2) * f->spc; }

static int fat_load(Fat32 *f, uint32_t sec) {
    if (f->fat_ok && f->fatsec == sec) return 0;
    if (rd(f, f->fat_lba + sec, 1, f->fatbuf)) {
        f->fat_ok = 0;
        return FS_EIO;
    }
    f->fatsec = sec;
    f->fat_ok = 1;
    return 0;
}

static uint32_t fat_get(Fat32 *f, uint32_t c) {
    uint32_t off = c * 4;
    if (fat_load(f, off / f->ss)) return BAD_CL;
    return u32(f->fatbuf + off % f->ss) & 0x0FFFFFFF;
}

static int fat_set(Fat32 *f, uint32_t c, uint32_t v) {
    uint32_t off = c * 4, sec = off / f->ss;
    int r = fat_load(f, sec);
    if (r) return r;
    uint8_t *p = f->fatbuf + off % f->ss;
    p32(p, (u32(p) & 0xF0000000u) | (v & 0x0FFFFFFFu));
    for (uint32_t i = 0; i < f->nfats; i++)
        if (f->dev->write(f->dev, f->fat_lba + (uint64_t)i * f->fat_secs + sec, 1, f->fatbuf)) return FS_EIO;
    return 0;
}

static int alloc_cluster(Fat32 *f, uint32_t *out) {
    uint32_t c = valid(f, f->next_free) ? f->next_free : 2;
    for (uint32_t n = 0; n < f->nclusters; n++) {
        if (fat_get(f, c) == 0) {
            int r = fat_set(f, c, EOC);
            if (r) return r;
            if (f->free_count) f->free_count--;
            f->next_free = c + 1;
            *out = c;
            return 0;
        }
        if (++c > f->nclusters + 1) c = 2;
    }
    return FS_ENOSPC;
}

static int zero_cluster(Fat32 *f, uint32_t c) {
    memset(f->clbuf, 0, f->cbytes);
    return wr(f, clba(f, c), f->spc, f->clbuf);
}

static int free_chain(Fat32 *f, uint32_t c) {
    for (uint32_t n = 0; valid(f, c) && n <= f->nclusters; n++) {
        uint32_t nx = fat_get(f, c);
        int r = fat_set(f, c, 0);
        if (r) return r;
        f->free_count++;
        if (c < f->next_free) f->next_free = c;
        c = nx;
    }
    return 0;
}

static uint32_t chain_len(Fat32 *f, uint32_t c) {
    uint32_t n = 0;
    while (valid(f, c) && n <= f->nclusters) {
        n++;
        c = fat_get(f, c);
    }
    return n;
}

static void flush_fsinfo(Fat32 *f) {
    if (!f->fsinfo) return;
    uint8_t *b = f->secbuf;   /* kurzzeitig als Puffer: Sektor-Cache danach ungueltig */
    f->sec_ok = 0;
    if (rd(f, f->fsinfo, 1, b)) return;
    p32(b + 488, f->free_count);
    p32(b + 492, f->next_free);
    f->dev->write(f->dev, f->fsinfo, 1, b);
}

/* ------------------------------------------------------------------ */
/* Verzeichniseintraege                                                */
/* ------------------------------------------------------------------ */
static int dir_locate(Fat32 *f, DirPos p, uint64_t *lba, uint32_t *off) {
    if (!valid(f, p.cluster)) return FS_EIO;
    *lba = clba(f, p.cluster) + (uint64_t)(p.index * 32) / f->ss;
    *off = (p.index * 32) % f->ss;
    return 0;
}

static int dir_read(Fat32 *f, DirPos p, uint8_t *e) {
    uint64_t lba; uint32_t off;
    int r = dir_locate(f, p, &lba, &off);
    if (r) return r;
    if (!(f->sec_ok && f->seclba == lba)) {
        if (rd(f, lba, 1, f->secbuf)) return FS_EIO;
        f->seclba = lba;
        f->sec_ok = 1;
    }
    memcpy(e, f->secbuf + off, 32);
    return 0;
}

static int dir_write(Fat32 *f, DirPos p, const uint8_t *e) {
    uint64_t lba; uint32_t off;
    int r = dir_locate(f, p, &lba, &off);
    if (r) return r;
    if (!(f->sec_ok && f->seclba == lba)) {
        if (rd(f, lba, 1, f->secbuf)) return FS_EIO;
        f->seclba = lba;
        f->sec_ok = 1;
    }
    memcpy(f->secbuf + off, e, 32);
    if (f->dev->write(f->dev, lba, 1, f->secbuf)) return FS_EIO;
    return 0;
}

/* naechster Eintrag; 1 = Ende des Verzeichnisses (extend: neuen Cluster anhaengen) */
static int dirpos_next(Fat32 *f, DirPos *p, int extend) {
    if (++p->index < f->epc) return 0;
    uint32_t nx = fat_get(f, p->cluster);
    if (!valid(f, nx)) {
        if (!extend) return 1;
        uint32_t nc;
        int r = alloc_cluster(f, &nc);
        if (r) return r;
        r = zero_cluster(f, nc);
        if (r) return r;
        r = fat_set(f, p->cluster, nc);
        if (r) return r;
        nx = nc;
    }
    p->cluster = nx;
    p->index = 0;
    return 0;
}

static uint32_t cluster_of(const uint8_t *e) { return ((uint32_t)u16(e + 20) << 16) | u16(e + 26); }

static uint8_t lfn_checksum(const uint8_t *n11) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + n11[i]);
    return s;
}

static const int lfn_off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static void short_to_name(const uint8_t *e, char *out) {
    int n = 0;
    int lb = e[12] & 0x08, le = e[12] & 0x10;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)e[i];
        if (lb && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[n++] = c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            if (le && c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[n++] = c;
        }
    }
    out[n] = 0;
}

typedef int (*ScanCb)(Fat32 *f, Found *fd, void *ctx);

/* Verzeichnis durchlaufen; cb != 0 beendet und wird durchgereicht. 0 = bis zum Ende, <0 = Fehler */
static int scan_dir(Fat32 *f, uint32_t dir, ScanCb cb, void *ctx) {
    DirPos p = {dir, 0};
    char lname[272];
    int total = 0, expect = 0, have = 0;
    uint8_t lck = 0;
    DirPos lstart = {0, 0};
    for (;;) {
        uint8_t e[32];
        int r = dir_read(f, p, e);
        if (r < 0) return r;
        if (e[0] == 0x00) return 0;
        if (e[0] == 0xE5) {
            have = 0;
            expect = 0;
        } else if (e[11] == ATTR_LFN) {
            uint8_t ord = e[0];
            if (ord & 0x40) {
                total = ord & 0x3F;
                if (total >= 1 && total <= 20) {
                    expect = total;
                    lck = e[13];
                    lstart = p;
                    have = 1;
                    memset(lname, 0, sizeof lname);
                } else {
                    have = 0;
                    expect = 0;
                }
            }
            if (have && (ord & 0x3F) == expect && e[13] == lck) {
                for (int k = 0; k < 13; k++) {
                    uint16_t ch = u16(e + lfn_off[k]);
                    if (ch == 0 || ch == 0xFFFF) break;
                    lname[(expect - 1) * 13 + k] = ch < 128 ? (char)ch : '?';
                }
                expect--;
            } else {
                have = 0;
                expect = 0;
            }
        } else if (e[11] & ATTR_VOL) {
            have = 0;
            expect = 0;
        } else {
            Found fd;
            memcpy(fd.e, e, 32);
            fd.spos = p;
            fd.dir = dir;
            if (have && expect == 0 && lfn_checksum(e) == lck) {
                scopy(fd.name, lname, sizeof fd.name);
                fd.lpos = lstart;
                fd.nent = (uint32_t)total + 1;
            } else {
                short_to_name(e, fd.name);
                fd.lpos = p;
                fd.nent = 1;
            }
            have = 0;
            expect = 0;
            int dots = e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '));
            if (!dots) {
                int rr = cb(f, &fd, ctx);
                if (rr) return rr;
            }
        }
        r = dirpos_next(f, &p, 0);
        if (r < 0) return r;
        if (r == 1) return 0;
    }
}

typedef struct {
    const char *name;
    Found *out;
} LookCtx;

static int look_cb(Fat32 *f, Found *fd, void *ctx) {
    (void)f;
    LookCtx *c = ctx;
    char sn[16];
    short_to_name(fd->e, sn);
    if (streq_ci(fd->name, c->name) || streq_ci(sn, c->name)) {
        *c->out = *fd;
        return 1;
    }
    return 0;
}

static int dir_lookup(Fat32 *f, uint32_t dir, const char *name, Found *out) {
    LookCtx c = {name, out};
    int r = scan_dir(f, dir, look_cb, &c);
    if (r == 1) return 0;
    return r < 0 ? r : FS_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Pfade                                                               */
/* ------------------------------------------------------------------ */
static int next_comp(const char **p, char *comp) {
    while (**p == '/') (*p)++;
    if (!**p) return 0;
    int n = 0;
    while (**p && **p != '/') {
        if (n < 255) comp[n++] = **p;
        (*p)++;
    }
    comp[n] = 0;
    return 1;
}

/* Pfad aufloesen. *is_root = 1 beim Wurzelverzeichnis (dann ist out nicht gefuellt) */
static int resolve(Fat32 *f, const char *path, Found *out, int *is_root) {
    uint32_t dir = f->root;
    const char *p = path;
    char comp[256];
    Found fd;
    int have = 0;
    *is_root = 1;
    while (next_comp(&p, comp)) {
        if (have) {
            if (!(fd.e[11] & ATTR_DIR)) return FS_ENOTDIR;
            dir = cluster_of(fd.e);
            if (dir == 0) dir = f->root;
        }
        int r = dir_lookup(f, dir, comp, &fd);
        if (r) return r;
        have = 1;
        *is_root = 0;
    }
    if (have) *out = fd;
    return 0;
}

static int resolve_dir(Fat32 *f, const char *path, uint32_t *cluster) {
    Found fd;
    int is_root;
    int r = resolve(f, path, &fd, &is_root);
    if (r) return r;
    if (is_root) {
        *cluster = f->root;
        return 0;
    }
    if (!(fd.e[11] & ATTR_DIR)) return FS_ENOTDIR;
    uint32_t c = cluster_of(fd.e);
    *cluster = c ? c : f->root;
    return 0;
}

/* "/a/b/c" -> parent "/a/b", leaf "c" */
static int split_leaf(const char *path, char *parent, char *leaf) {
    size_t n = slen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t i = n;
    while (i > 0 && path[i - 1] != '/') i--;
    if (i >= n) return FS_EINVAL;   /* Wurzel hat keinen Namen */
    if (n - i > 255) return FS_ENAME;
    for (size_t k = 0; k < n - i; k++) leaf[k] = path[i + k];
    leaf[n - i] = 0;
    if (i <= 1) {
        parent[0] = '/';
        parent[1] = 0;
    } else {
        for (size_t k = 0; k < i - 1; k++) parent[k] = path[k];
        parent[i - 1] = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Namen: 8.3, Aliase, lange Namen                                     */
/* ------------------------------------------------------------------ */
static int valid_name(const char *n) {
    size_t len = slen(n);
    if (len == 0 || len > 255) return 0;
    if (n[0] == ' ' || n[len - 1] == ' ' || n[len - 1] == '.') return 0;
    if ((len == 1 && n[0] == '.') || (len == 2 && n[0] == '.' && n[1] == '.')) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c < 32 || c > 126) return 0;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    return 1;
}

static int short_char_ok(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    const char *ok = "$%'-_@~!(){}^#&";
    for (; *ok; ok++)
        if (*ok == c) return 1;
    return 0;
}

/* passt der Name ohne LFN in 8.3 (mit NT-Kleinschreibungsflags)? */
static int fits_83(const char *name, uint8_t sn[11], uint8_t *nt) {
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.') dot = p;
    size_t blen = dot ? (size_t)(dot - name) : slen(name);
    size_t elen = dot ? slen(dot + 1) : 0;
    if (blen == 0 || blen > 8 || elen > 3) return 0;
    int bup = 0, blo = 0, eup = 0, elo = 0;
    memset(sn, ' ', 11);
    for (size_t i = 0; i < blen; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') { blo = 1; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') bup = 1;
        if (!short_char_ok(c)) return 0;
        sn[i] = (uint8_t)c;
    }
    for (size_t i = 0; i < elen; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') { elo = 1; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') eup = 1;
        if (!short_char_ok(c)) return 0;
        sn[8 + i] = (uint8_t)c;
    }
    if ((bup && blo) || (eup && elo)) return 0;   /* gemischte Schreibweise braucht LFN */
    *nt = (uint8_t)((blo ? 0x08 : 0) | (elo ? 0x10 : 0));
    return 1;
}

typedef struct {
    const uint8_t *sn;
    int found;
} ShortCtx;

static int short_cb(Fat32 *f, Found *fd, void *ctx) {
    (void)f;
    ShortCtx *c = ctx;
    for (int i = 0; i < 11; i++)
        if (fd->e[i] != c->sn[i]) return 0;
    c->found = 1;
    return 1;
}

static int short_exists(Fat32 *f, uint32_t dir, const uint8_t *sn) {
    ShortCtx c = {sn, 0};
    scan_dir(f, dir, short_cb, &c);
    return c.found;
}

/* Alias im Stil "NAMENS~1.TXT" erzeugen, eindeutig im Verzeichnis */
static void make_alias(Fat32 *f, uint32_t dir, const char *name, uint8_t sn[11]) {
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.') dot = p;
    char base[16], ext[4];
    int bn = 0, en = 0;
    size_t blen = dot ? (size_t)(dot - name) : slen(name);
    for (size_t i = 0; i < blen && bn < 8; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!short_char_ok(c)) c = '_';
        base[bn++] = c;
    }
    if (dot) {
        for (const char *p = dot + 1; *p && en < 3; p++) {
            char c = *p;
            if (c == ' ') continue;
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (!short_char_ok(c)) c = '_';
            ext[en++] = c;
        }
    }
    if (bn == 0) base[bn++] = '_';
    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[8];
        int tn = 0;
        tail[tn++] = '~';
        char dig[8];
        int dn = 0;
        for (uint32_t t = n; t; t /= 10) dig[dn++] = (char)('0' + t % 10);
        while (dn) tail[tn++] = dig[--dn];
        int keep = 8 - tn;
        if (keep > bn) keep = bn;
        memset(sn, ' ', 11);
        for (int i = 0; i < keep; i++) sn[i] = (uint8_t)base[i];
        for (int i = 0; i < tn; i++) sn[keep + i] = (uint8_t)tail[i];
        for (int i = 0; i < en; i++) sn[8 + i] = (uint8_t)ext[i];
        if (!short_exists(f, dir, sn)) return;
    }
}

/* Eintraege (LFN + Kurzeintrag) in 'dir' anlegen. longname == 0: nur Kurzeintrag. */
static int dir_create(Fat32 *f, uint32_t dir, const uint8_t sn[11], uint8_t nt, uint8_t attr,
                      uint32_t first, uint32_t size, const char *longname, Found *out) {
    int nlfn = longname ? (int)((slen(longname) + 12) / 13) : 0;
    int need = nlfn + 1;

    DirPos p = {dir, 0}, run_start = {dir, 0};
    int run = 0;
    for (;;) {
        uint8_t e[32];
        int r = dir_read(f, p, e);
        if (r < 0) return r;
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (!run) run_start = p;
            if (++run == need) break;
        } else {
            run = 0;
        }
        r = dirpos_next(f, &p, 1);   /* bei Bedarf Verzeichnis um einen Cluster verlaengern */
        if (r < 0) return r;
    }

    uint16_t date, time;
    fat_now(&date, &time);
    uint8_t ck = lfn_checksum(sn);
    size_t len = longname ? slen(longname) : 0;
    DirPos q = run_start;
    for (int j = 0; j < nlfn; j++) {
        int ord = nlfn - j;
        uint8_t e[32];
        memset(e, 0, 32);
        e[0] = (uint8_t)(ord | (j == 0 ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = ck;
        for (int k = 0; k < 13; k++) {
            size_t idx = (size_t)(ord - 1) * 13 + (size_t)k;
            uint16_t ch = idx < len ? (uint16_t)(uint8_t)longname[idx] : (idx == len ? 0x0000 : 0xFFFF);
            p16(e + lfn_off[k], ch);
        }
        int r = dir_write(f, q, e);
        if (r) return r;
        r = dirpos_next(f, &q, 0);
        if (r < 0) return r;
    }
    uint8_t e[32];
    memset(e, 0, 32);
    memcpy(e, sn, 11);
    e[11] = attr;
    e[12] = nt;
    p16(e + 14, time); p16(e + 16, date);   /* angelegt */
    p16(e + 18, date);                      /* letzter Zugriff */
    p16(e + 20, (uint16_t)(first >> 16));
    p16(e + 22, time); p16(e + 24, date);   /* geaendert */
    p16(e + 26, (uint16_t)first);
    p32(e + 28, size);
    int r = dir_write(f, q, e);
    if (r) return r;

    if (out) {
        memcpy(out->e, e, 32);
        out->spos = q;
        out->lpos = run_start;
        out->nent = (uint32_t)need;
        out->dir = dir;
        scopy(out->name, longname ? longname : "", sizeof out->name);
    }
    return 0;
}

static int create_entry(Fat32 *f, uint32_t dir, const char *name, uint8_t attr, uint32_t first, uint32_t size, Found *out) {
    if (!valid_name(name)) return FS_ENAME;
    uint8_t sn[11], nt = 0;
    if (fits_83(name, sn, &nt)) {
        if (short_exists(f, dir, sn)) return FS_EEXIST;
        return dir_create(f, dir, sn, nt, attr, first, size, 0, out);
    }
    make_alias(f, dir, name, sn);
    return dir_create(f, dir, sn, 0, attr, first, size, name, out);
}

static int entry_update(Fat32 *f, Found *fd, uint32_t first, uint32_t size) {
    uint16_t date, time;
    fat_now(&date, &time);
    uint8_t *e = fd->e;
    p16(e + 20, (uint16_t)(first >> 16));
    p16(e + 26, (uint16_t)first);
    p32(e + 28, size);
    p16(e + 22, time); p16(e + 24, date); p16(e + 18, date);
    if (!(e[11] & ATTR_DIR)) e[11] |= ATTR_ARC;
    return dir_write(f, fd->spos, e);
}

static int entries_delete(Fat32 *f, Found *fd) {
    DirPos p = fd->lpos;
    for (uint32_t i = 0; i < fd->nent; i++) {
        uint8_t e[32];
        int r = dir_read(f, p, e);
        if (r) return r;
        e[0] = 0xE5;
        r = dir_write(f, p, e);
        if (r) return r;
        r = dirpos_next(f, &p, 0);
        if (r < 0) return r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dateiinhalt schreiben                                               */
/* ------------------------------------------------------------------ */
/* Kette so verlaengern, dass sie off+len Bytes fasst, dann Daten schreiben */
static int file_write_range(Fat32 *f, uint32_t *first, uint32_t *size, uint32_t off, const uint8_t *data, uint32_t len) {
    if (len == 0) return 0;
    uint32_t end = off + len;
    if (end < off) return FS_ENOSPC;
    uint32_t need = (uint32_t)(((uint64_t)end + f->cbytes - 1) / f->cbytes);

    uint32_t cur = *first, prev = 0;
    for (uint32_t n = 0; n < need; n++) {
        if (!valid(f, cur)) {
            uint32_t nc;
            int r = alloc_cluster(f, &nc);
            if (r) return r;
            r = zero_cluster(f, nc);
            if (r) return r;
            if (prev) r = fat_set(f, prev, nc);
            else *first = nc;
            if (r) return r;
            cur = nc;
        }
        prev = cur;
        cur = fat_get(f, cur);
    }

    uint32_t c = *first;
    for (uint32_t skip = off / f->cbytes; skip; skip--) {
        c = fat_get(f, c);
        if (!valid(f, c)) return FS_EIO;
    }
    uint32_t pos = off % f->cbytes, done = 0;
    while (done < len) {
        if (!valid(f, c)) return FS_EIO;
        uint32_t chunk = f->cbytes - pos;
        if (chunk > len - done) chunk = len - done;
        if (chunk < f->cbytes) {
            if (rd(f, clba(f, c), f->spc, f->clbuf)) return FS_EIO;
        }
        memcpy(f->clbuf + pos, data + done, chunk);
        if (wr(f, clba(f, c), f->spc, f->clbuf)) return FS_EIO;
        done += chunk;
        pos = 0;
        if (done < len) c = fat_get(f, c);
    }
    if (end > *size) *size = end;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fs-Operationen                                                      */
/* ------------------------------------------------------------------ */
static void fill_entry(const Found *fd, FsEntry *out) {
    scopy(out->name, fd->name, sizeof out->name);
    out->is_dir = (fd->e[11] & ATTR_DIR) ? 1 : 0;
    out->size = out->is_dir ? 0 : u32(fd->e + 28);
}

static int f_stat(Fs *fs, const char *path, FsEntry *out) {
    Fat32 *f = fs->priv;
    Found fd;
    int is_root;
    int r = resolve(f, path, &fd, &is_root);
    if (r) return r;
    if (is_root) {
        out->name[0] = 0;
        out->size = 0;
        out->is_dir = 1;
        return 0;
    }
    fill_entry(&fd, out);
    return 0;
}

typedef struct {
    FsDirCb cb;
    void *ctx;
} ListCtx;

static int list_cb(Fat32 *f, Found *fd, void *ctx) {
    (void)f;
    ListCtx *l = ctx;
    FsEntry e;
    fill_entry(fd, &e);
    return l->cb(&e, l->ctx) ? 2 : 0;
}

static int f_readdir(Fs *fs, const char *path, FsDirCb cb, void *ctx) {
    Fat32 *f = fs->priv;
    uint32_t dir;
    int r = resolve_dir(f, path, &dir);
    if (r) return r;
    ListCtx l = {cb, ctx};
    r = scan_dir(f, dir, list_cb, &l);
    return r < 0 ? r : 0;
}

static int64_t f_read(Fs *fs, const char *path, uint64_t off, void *buf, uint64_t len) {
    Fat32 *f = fs->priv;
    Found fd;
    int is_root;
    int r = resolve(f, path, &fd, &is_root);
    if (r) return r;
    if (is_root || (fd.e[11] & ATTR_DIR)) return FS_EISDIR;
    uint32_t size = u32(fd.e + 28);
    if (off >= size) return 0;
    if (len > size - off) len = size - off;

    uint32_t c = cluster_of(fd.e);
    for (uint64_t skip = off / f->cbytes; skip; skip--) {
        if (!valid(f, c)) return FS_EIO;
        c = fat_get(f, c);
    }
    uint32_t pos = (uint32_t)(off % f->cbytes);
    uint64_t done = 0;
    while (done < len) {
        if (!valid(f, c)) return FS_EIO;
        if (rd(f, clba(f, c), f->spc, f->clbuf)) return FS_EIO;
        uint64_t chunk = f->cbytes - pos;
        if (chunk > len - done) chunk = len - done;
        memcpy((uint8_t *)buf + done, f->clbuf + pos, (size_t)chunk);
        done += chunk;
        pos = 0;
        if (done < len) c = fat_get(f, c);
    }
    return (int64_t)done;
}

static int f_write(Fs *fs, const char *path, const void *buf, uint64_t len, int append) {
    Fat32 *f = fs->priv;
    char parent[512], leaf[256];
    int r = split_leaf(path, parent, leaf);
    if (r) return r;
    uint32_t dir;
    r = resolve_dir(f, parent, &dir);
    if (r) return r;

    Found fd;
    int exists = dir_lookup(f, dir, leaf, &fd);
    if (exists < 0 && exists != FS_ENOENT) return exists;
    exists = (exists == 0);

    uint32_t first = 0, size = 0;
    if (exists) {
        if (fd.e[11] & ATTR_DIR) return FS_EISDIR;
        first = cluster_of(fd.e);
        size = u32(fd.e + 28);
    }
    uint64_t base = (exists && append) ? size : 0;
    if (base + len > 0xFFFFFFFFULL) return FS_ENOSPC;

    /* Platz vorab pruefen */
    uint64_t total_need = (base + len + f->cbytes - 1) / f->cbytes;
    uint64_t have = (exists && first) ? chain_len(f, first) : 0;
    if (!append || !exists) {
        if (total_need > have && total_need - have > f->free_count) return FS_ENOSPC;
    } else if (total_need > have && total_need - have > f->free_count) {
        return FS_ENOSPC;
    }

    if (exists && !append) {   /* ersetzen: alte Kette freigeben */
        if (first) {
            r = free_chain(f, first);
            if (r) return r;
        }
        first = 0;
        size = 0;
    }
    if (!exists) {
        r = create_entry(f, dir, leaf, ATTR_ARC, 0, 0, &fd);
        if (r) return r;
    }
    r = file_write_range(f, &first, &size, (uint32_t)base, buf, (uint32_t)len);
    int r2 = entry_update(f, &fd, first, size);
    flush_fsinfo(f);
    return r ? r : r2;
}

static int f_mkdir(Fs *fs, const char *path) {
    Fat32 *f = fs->priv;
    char parent[512], leaf[256];
    int r = split_leaf(path, parent, leaf);
    if (r) return r;
    uint32_t dir;
    r = resolve_dir(f, parent, &dir);
    if (r) return r;
    Found tmp;
    int ex = dir_lookup(f, dir, leaf, &tmp);
    if (ex == 0) return FS_EEXIST;
    if (ex != FS_ENOENT) return ex;

    uint32_t nc;
    r = alloc_cluster(f, &nc);
    if (r) return r;
    r = zero_cluster(f, nc);
    if (r) {
        free_chain(f, nc);
        return r;
    }
    Found fd;
    r = create_entry(f, dir, leaf, ATTR_DIR, nc, 0, &fd);
    if (r) {
        free_chain(f, nc);
        flush_fsinfo(f);
        return r;
    }
    /* "." und ".." im neuen Verzeichnis */
    uint16_t date, time;
    fat_now(&date, &time);
    uint8_t e[32];
    DirPos p = {nc, 0};
    for (int i = 0; i < 2; i++) {
        memset(e, 0, 32);
        memset(e, ' ', 11);
        e[0] = '.';
        if (i == 1) e[1] = '.';
        e[11] = ATTR_DIR;
        uint32_t cl = (i == 0) ? nc : (dir == f->root ? 0 : dir);
        p16(e + 14, time); p16(e + 16, date); p16(e + 18, date);
        p16(e + 20, (uint16_t)(cl >> 16)); p16(e + 22, time); p16(e + 24, date); p16(e + 26, (uint16_t)cl);
        r = dir_write(f, p, e);
        if (r) return r;
        p.index++;
    }
    flush_fsinfo(f);
    return 0;
}

static int empty_cb(Fat32 *f, Found *fd, void *ctx) {
    (void)f; (void)fd; (void)ctx;
    return 1;
}

static int f_remove(Fs *fs, const char *path) {
    Fat32 *f = fs->priv;
    Found fd;
    int is_root;
    int r = resolve(f, path, &fd, &is_root);
    if (r) return r;
    if (is_root) return FS_EINVAL;
    uint32_t first = cluster_of(fd.e);
    if (fd.e[11] & ATTR_DIR) {
        if (first) {
            int s = scan_dir(f, first, empty_cb, 0);
            if (s < 0) return s;
            if (s == 1) return FS_ENOTEMPTY;
        }
    }
    r = entries_delete(f, &fd);
    if (r) return r;
    if (first) r = free_chain(f, first);
    flush_fsinfo(f);
    return r;
}

static int f_rename(Fs *fs, const char *from, const char *to) {
    Fat32 *f = fs->priv;
    Found src;
    int is_root;
    int r = resolve(f, from, &src, &is_root);
    if (r) return r;
    if (is_root) return FS_EINVAL;

    size_t fl = slen(from);
    if ((src.e[11] & ATTR_DIR) && slen(to) > fl) {   /* Verzeichnis nicht in sich selbst verschieben */
        int prefix = 1;
        for (size_t i = 0; i < fl; i++)
            if (lc(from[i]) != lc(to[i])) { prefix = 0; break; }
        if (prefix && to[fl] == '/') return FS_EINVAL;
    }

    char parent[512], leaf[256];
    r = split_leaf(to, parent, leaf);
    if (r) return r;
    uint32_t dir;
    r = resolve_dir(f, parent, &dir);
    if (r) return r;
    Found other;
    int ex = dir_lookup(f, dir, leaf, &other);
    if (ex == 0 && !(other.spos.cluster == src.spos.cluster && other.spos.index == src.spos.index)) return FS_EEXIST;
    if (ex != 0 && ex != FS_ENOENT) return ex;

    uint32_t first = cluster_of(src.e), size = u32(src.e + 28);
    uint8_t attr = src.e[11];
    Found nf;
    r = create_entry(f, dir, leaf, attr, first, size, &nf);
    if (r) return r;
    r = entries_delete(f, &src);
    if (r) return r;
    if ((attr & ATTR_DIR) && first) {   /* ".." des verschobenen Verzeichnisses anpassen */
        DirPos dd = {first, 1};
        uint8_t e[32];
        if (dir_read(f, dd, e) == 0 && e[0] == '.' && e[1] == '.') {
            uint32_t cl = (dir == f->root) ? 0 : dir;
            p16(e + 20, (uint16_t)(cl >> 16));
            p16(e + 26, (uint16_t)cl);
            dir_write(f, dd, e);
        }
    }
    flush_fsinfo(f);
    return 0;
}

static int f_statfs(Fs *fs, uint64_t *total, uint64_t *free_b) {
    Fat32 *f = fs->priv;
    *total = (uint64_t)f->nclusters * f->cbytes;
    *free_b = (uint64_t)f->free_count * f->cbytes;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Einbinden                                                           */
/* ------------------------------------------------------------------ */
static uint32_t count_free(Fat32 *f) {
    uint32_t chunk_secs = 64;
    uint8_t *buf = kapi.malloc((size_t)chunk_secs * f->ss);
    if (!buf) return 0;
    uint32_t cnt = 0;
    for (uint32_t s = 0; s < f->fat_secs; s += chunk_secs) {
        uint32_t n = f->fat_secs - s < chunk_secs ? f->fat_secs - s : chunk_secs;
        if (rd(f, f->fat_lba + s, n, buf)) break;
        for (uint32_t i = 0; i < n * f->ss / 4; i++) {
            uint32_t c = s * f->ss / 4 + i;
            if (c < 2 || c > f->nclusters + 1) continue;
            if ((u32(buf + i * 4) & 0x0FFFFFFF) == 0) cnt++;
        }
    }
    kapi.free(buf);
    return cnt;
}

/* Rueckgabe 0 und *out gesetzt, sonst negativer Fehler. Eingebunden wird schreibgeschuetzt. */
int fat32_mount(BlockDev *bd, Fs **out) {
    uint8_t *bs = kapi.malloc(bd->sector_size);
    if (!bs) return FS_EIO;
    if (bd->read(bd, 0, 1, bs)) {
        kapi.free(bs);
        return FS_EIO;
    }
    uint32_t ss = u16(bs + 11), spc = bs[13], reserved = u16(bs + 14), nfats = bs[16];
    uint32_t fatsz = u32(bs + 36);
    uint32_t tot = u16(bs + 19) ? u16(bs + 19) : u32(bs + 32);
    int ok = bs[510] == 0x55 && bs[511] == 0xAA && ss == bd->sector_size && spc && (spc & (spc - 1)) == 0 &&
             nfats >= 1 && nfats <= 2 && u16(bs + 17) == 0 && u16(bs + 22) == 0 && fatsz && tot && reserved;
    if (!ok || (uint64_t)ss * spc > 65536) {
        kapi.free(bs);
        return FS_EINVAL;
    }
    uint32_t data_start = reserved + nfats * fatsz;
    if (tot <= data_start) {
        kapi.free(bs);
        return FS_EINVAL;
    }

    Fat32 *f = kapi.malloc(sizeof(Fat32));
    Fs *fs = kapi.malloc(sizeof(Fs));
    if (!f || !fs) {
        kapi.free(bs); kapi.free(f); kapi.free(fs);
        return FS_EIO;
    }
    memset(f, 0, sizeof *f);
    memset(fs, 0, sizeof *fs);
    f->dev = bd;
    f->ss = ss;
    f->spc = spc;
    f->cbytes = ss * spc;
    f->epc = f->cbytes / 32;
    f->fat_lba = reserved;
    f->fat_secs = fatsz;
    f->nfats = nfats;
    f->data_lba = data_start;
    f->root = u32(bs + 44);
    f->nclusters = (tot - data_start) / spc;
    f->fsinfo = u16(bs + 48);
    if (f->fsinfo >= reserved) f->fsinfo = 0;
    f->fatbuf = kapi.malloc(ss);
    f->secbuf = kapi.malloc(ss);
    f->clbuf = kapi.malloc(f->cbytes);
    if (!f->fatbuf || !f->secbuf || !f->clbuf || !valid(f, f->root)) {
        kapi.free(f->fatbuf); kapi.free(f->secbuf); kapi.free(f->clbuf);
        kapi.free(bs); kapi.free(f); kapi.free(fs);
        return FS_EINVAL;
    }
    f->next_free = 2;
    if (f->fsinfo && rd(f, f->fsinfo, 1, f->secbuf) == 0 && u32(f->secbuf) == 0x41615252 &&
        u32(f->secbuf + 484) == 0x61417272) {
        uint32_t nf = u32(f->secbuf + 492);
        if (valid(f, nf)) f->next_free = nf;
    }
    f->free_count = count_free(f);

    char label[12];
    for (int i = 0; i < 11; i++) label[i] = (char)bs[71 + i];
    label[11] = 0;
    for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = 0;
    scopy(fs->label, label[0] ? label : "NO NAME", sizeof fs->label);
    scopy(fs->type, "FAT32", sizeof fs->type);
    fs->dev = bd;
    fs->priv = f;
    fs->readonly = 1;
    fs->stat = f_stat;
    fs->readdir = f_readdir;
    fs->read = f_read;
    fs->write = f_write;
    fs->mkdir = f_mkdir;
    fs->remove = f_remove;
    fs->rename = f_rename;
    fs->statfs = f_statfs;
    kapi.free(bs);
    *out = fs;
    return 0;
}
