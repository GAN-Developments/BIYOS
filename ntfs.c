/* SPDX-License-Identifier: MIT */
/*
 * ntfs.c - NTFS-Dateisystem: Lesen und (experimentell) Schreiben
 *
 * Unterstuetzt: MFT mit Fixups, Attributlisten (Erweiterungs-Records), residente und nicht
 * residente Attribute mit Runlists (inkl. Sparse), Verzeichnisse als B+-Baum ($INDEX_ROOT und
 * $INDEX_ALLOCATION) mit Namensvergleich ueber $UpCase, benannte Datenstroeme ("datei:strom"),
 * Hardlinks (mehrere Namen zeigen auf denselben Record), Volumenname.
 * Nicht unterstuetzt: komprimierte und verschluesselte Dateien (Lesefehler "nicht unterstuetzt"),
 * Reparse-Points werden wie normale Dateien behandelt, nicht-ASCII-Namen erscheinen als '?'.
 * Das Journal ($LogFile) wird beim Lesen nicht ausgewertet: ein "unsauberes" Volume (z.B. Windows-Schnellstart)
 * kann veraltete Daten zeigen. Schreiben (write/mkdir/remove/rename) ist EXPERIMENTELL, siehe "Schreiben" unten.
 */
#include "kernel.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);

#define SPARSE_LCN (~0ULL)
#define REF_MASK   0x0000FFFFFFFFFFFFULL

#define AT_STD_INFO   0x10
#define AT_ATTR_LIST  0x20
#define AT_FILE_NAME  0x30
#define AT_VOL_NAME   0x60
#define AT_DATA       0x80
#define AT_INDEX_ROOT 0x90
#define AT_INDEX_ALLOC 0xA0
#define AT_END        0xFFFFFFFFu

typedef struct {
    uint64_t vcn, len, lcn;
} Run;

typedef struct {
    int resident, compressed, encrypted, have_size;
    uint8_t *rdata;
    uint32_t rlen;
    Run *runs;
    uint32_t nruns, cap;
    uint64_t real_size, init_size;
} Attr;

typedef struct {
    BlockDev *dev;
    uint32_t bps, spc, cbytes, rec_size;
    uint64_t total_sectors, mft_lcn, clusters;
    Attr mft;
    uint16_t *upcase;
    uint32_t upcase_n;
    /* nur fuer das Schreiben */
    uint64_t mftmirr_lcn, alloc_hint, mft_records, bm_bytes, mbm_bytes;
    uint32_t idx_size;
    int wready;
    uint8_t *bm, *mbm;
    Attr bm_attr, mbm_attr;
} Ntfs;

typedef struct {
    Attr root, alloc;
    int has_alloc;
    uint32_t bsize;
} Dir;

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, size_t max) {
    size_t i = 0;
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int memeq(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    while (n--)
        if (*p++ != *q++) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Geraet                                                              */
/* ------------------------------------------------------------------ */
static int rd_bytes(Ntfs *n, uint64_t off, void *buf, uint64_t len) {
    uint32_t ss = n->dev->sector_size;
    uint8_t *out = buf;
    uint8_t *tmp = 0;
    int r = 0;
    uint32_t head = (uint32_t)(off % ss);
    if (head || (len % ss)) {
        tmp = kapi.malloc(ss);
        if (!tmp) return FS_ENOSPC;
    }
    if (head && len) {
        if (n->dev->read(n->dev, off / ss, 1, tmp)) { r = FS_EIO; goto done; }
        uint64_t c = ss - head;
        if (c > len) c = len;
        memcpy(out, tmp + head, (size_t)c);
        off += c; out += c; len -= c;
    }
    uint64_t mid = len - len % ss;
    if (mid) {
        if (n->dev->read(n->dev, off / ss, (uint32_t)(mid / ss), out)) { r = FS_EIO; goto done; }
        off += mid; out += mid; len -= mid;
    }
    if (len) {
        if (n->dev->read(n->dev, off / ss, 1, tmp)) { r = FS_EIO; goto done; }
        memcpy(out, tmp, (size_t)len);
    }
done:
    kapi.free(tmp);
    return r;
}

static int apply_fixups(uint8_t *rec, uint32_t size, const char *magic) {
    if (!memeq(rec, magic, 4)) return FS_EIO;
    uint32_t uo = le16(rec + 4), uc = le16(rec + 6);
    if (uc < 2 || uo + 2 * uc > size) return FS_EIO;
    uint16_t usn = le16(rec + uo);
    for (uint32_t i = 1; i < uc; i++) {
        uint32_t pos = i * 512 - 2;
        if (pos + 2 > size) return FS_EIO;
        if (le16(rec + pos) != usn) return FS_EIO;   /* beschaedigter Record */
        rec[pos] = rec[uo + 2 * i];
        rec[pos + 1] = rec[uo + 2 * i + 1];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Attribute                                                           */
/* ------------------------------------------------------------------ */
static void attr_free(Attr *a) {
    kapi.free(a->rdata);
    kapi.free(a->runs);
    memset(a, 0, sizeof *a);
}

static int run_add(Attr *a, uint64_t vcn, uint64_t len, uint64_t lcn) {
    if (a->nruns == a->cap) {
        uint32_t nc = a->cap ? a->cap * 2 : 8;
        Run *r = kapi.realloc(a->runs, nc * sizeof(Run));
        if (!r) return FS_ENOSPC;
        a->runs = r;
        a->cap = nc;
    }
    a->runs[a->nruns].vcn = vcn;
    a->runs[a->nruns].len = len;
    a->runs[a->nruns].lcn = lcn;
    a->nruns++;
    return 0;
}

static int decode_runs(Attr *a, const uint8_t *p, const uint8_t *end, uint64_t vcn) {
    int64_t lcn = 0;
    while (p < end && *p) {
        uint8_t h = *p++;
        int ls = h & 15, os = h >> 4;
        if (ls == 0 || ls > 8 || os > 8 || p + ls + os > end) return FS_EIO;
        uint64_t len = 0;
        for (int i = 0; i < ls; i++) len |= (uint64_t)p[i] << (8 * i);
        p += ls;
        int r;
        if (os == 0) {
            r = run_add(a, vcn, len, SPARSE_LCN);
        } else {
            uint64_t off = 0;
            for (int i = 0; i < os; i++) off |= (uint64_t)p[i] << (8 * i);
            if (os < 8 && (p[os - 1] & 0x80)) off |= ~0ULL << (8 * os);   /* Vorzeichen erweitern */
            p += os;
            lcn += (int64_t)off;
            r = run_add(a, vcn, len, (uint64_t)lcn);
        }
        if (r) return r;
        vcn += len;
    }
    return 0;
}

static int attr_name_eq(const uint8_t *a, const char *name) {
    uint32_t nl = a[9];
    if (!name) return nl == 0;
    if (nl != slen(name)) return 0;
    const uint8_t *p = a + le16(a + 10);
    for (uint32_t i = 0; i < nl; i++) {
        uint16_t c = le16(p + 2 * i);
        char x = (char)c, y = name[i];
        if (c > 127) return 0;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static int process_attr(Attr *out, const uint8_t *a) {
    if (!a[8]) {   /* resident */
        uint32_t vlen = le32(a + 0x10), voff = le16(a + 0x14);
        if (voff + vlen > le32(a + 4)) return FS_EIO;
        uint8_t *d = kapi.malloc((size_t)vlen + 1);
        if (!d) return FS_ENOSPC;
        memcpy(d, a + voff, vlen);
        d[vlen] = 0;
        kapi.free(out->rdata);
        out->rdata = d;
        out->rlen = vlen;
        out->resident = 1;
        out->real_size = vlen;
        out->have_size = 1;
        return 0;
    }
    uint16_t flags = le16(a + 12);
    if (flags & 0x0001) out->compressed = 1;
    if (flags & 0x4000) out->encrypted = 1;
    uint64_t start = le64(a + 0x10);
    if (start == 0) {
        out->real_size = le64(a + 0x30);
        out->init_size = le64(a + 0x38);
        out->have_size = 1;
    }
    uint32_t ro = le16(a + 0x20), alen = le32(a + 4);
    if (ro >= alen) return FS_EIO;
    return decode_runs(out, a + ro, a + alen, start);
}

static int read_record(Ntfs *n, uint64_t recno, uint8_t *buf);
static int64_t attr_read(Ntfs *n, Attr *a, uint64_t off, void *buf, uint64_t len);

static void sort_runs(Attr *a) {
    for (uint32_t i = 1; i < a->nruns; i++) {
        Run t = a->runs[i];
        uint32_t j = i;
        while (j > 0 && a->runs[j - 1].vcn > t.vcn) {
            a->runs[j] = a->runs[j - 1];
            j--;
        }
        a->runs[j] = t;
    }
}

/* Attribut (Typ, optional benannter Strom) eines Records laden, inkl. Erweiterungs-Records */
static int attr_load(Ntfs *n, uint64_t recno, uint32_t type, const char *name, Attr *out) {
    if (out != &n->mft) memset(out, 0, sizeof *out);
    uint8_t *rec = kapi.malloc(n->rec_size);
    if (!rec) return FS_ENOSPC;
    int r = read_record(n, recno, rec);
    if (r) { kapi.free(rec); return r; }

    int found = 0;
    Attr list;
    memset(&list, 0, sizeof list);
    int have_list = 0;
    uint32_t used = le32(rec + 0x18);
    if (used > n->rec_size) used = n->rec_size;
    for (const uint8_t *a = rec + le16(rec + 0x14); a + 16 <= rec + used && le32(a) != AT_END; a += le32(a + 4)) {
        uint32_t al = le32(a + 4);
        if (al < 16 || a + al > rec + used) break;
        uint32_t t = le32(a);
        if (t == AT_ATTR_LIST && !have_list) {
            if (process_attr(&list, a) == 0) have_list = 1;
        }
        if (t == type && attr_name_eq(a, name)) {
            r = process_attr(out, a);
            if (r) goto fail;
            found = 1;
        }
    }
    if (have_list) {
        uint8_t *lb = list.resident ? list.rdata : kapi.malloc((size_t)list.real_size + 1);
        int64_t ll = list.resident ? (int64_t)list.rlen : -1;
        if (!list.resident) {
            if (!lb) { r = FS_ENOSPC; goto fail; }
            ll = attr_read(n, &list, 0, lb, list.real_size);
        }
        uint8_t *ext = 0;
        for (int64_t off = 0; ll > 0 && off + 0x1A <= ll;) {
            const uint8_t *e = lb + off;
            uint32_t elen = le16(e + 4);
            if (elen < 0x1A) break;
            if (le32(e) == type) {
                int match = 1;
                uint32_t nl = e[6];
                if (!name) match = nl == 0;
                else {
                    match = nl == slen(name);
                    for (uint32_t i = 0; match && i < nl; i++) {
                        char x = (char)le16(e + e[7] + 2 * i), y = name[i];
                        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
                        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
                        if (x != y) match = 0;
                    }
                }
                uint64_t ref = le64(e + 0x10) & REF_MASK;
                if (match && ref != recno) {
                    if (!ext) ext = kapi.malloc(n->rec_size);
                    if (!ext) { r = FS_ENOSPC; break; }
                    if (read_record(n, ref, ext) == 0) {
                        uint32_t eused = le32(ext + 0x18);
                        if (eused > n->rec_size) eused = n->rec_size;
                        for (const uint8_t *a = ext + le16(ext + 0x14); a + 16 <= ext + eused && le32(a) != AT_END; a += le32(a + 4)) {
                            uint32_t al = le32(a + 4);
                            if (al < 16 || a + al > ext + eused) break;
                            if (le32(a) == type && attr_name_eq(a, name) && le16(a + 0xE) == le16(e + 0x18)) {
                                if (process_attr(out, a) == 0) found = 1;
                                break;
                            }
                        }
                    }
                }
            }
            off += elen;
        }
        kapi.free(ext);
        if (!list.resident) kapi.free(lb);
    }
    attr_free(&list);
    kapi.free(rec);
    sort_runs(out);
    return found ? 0 : (r ? r : FS_ENOENT);
fail:
    attr_free(&list);
    kapi.free(rec);
    return r;
}

static int64_t attr_read(Ntfs *n, Attr *a, uint64_t off, void *buf, uint64_t len) {
    if (a->compressed || a->encrypted) return FS_ENOSYS;
    uint64_t size = a->real_size;
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
    if (a->resident) {
        memcpy(buf, a->rdata + off, (size_t)len);
        return (int64_t)len;
    }
    uint8_t *out = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t vcn = pos / n->cbytes;
        uint64_t in = pos % n->cbytes;
        const Run *r = 0;
        for (uint32_t i = 0; i < a->nruns; i++)
            if (vcn >= a->runs[i].vcn && vcn < a->runs[i].vcn + a->runs[i].len) {
                r = &a->runs[i];
                break;
            }
        uint64_t chunk;
        if (!r) {   /* Luecke ausserhalb der Runs: wie Sparse behandeln */
            chunk = n->cbytes - in;
            if (chunk > len - done) chunk = len - done;
            memset(out + done, 0, (size_t)chunk);
        } else {
            uint64_t avail = (r->vcn + r->len - vcn) * n->cbytes - in;
            chunk = avail < len - done ? avail : len - done;
            if (r->lcn == SPARSE_LCN) memset(out + done, 0, (size_t)chunk);
            else {
                int rr = rd_bytes(n, (r->lcn + (vcn - r->vcn)) * n->cbytes + in, out + done, chunk);
                if (rr) return rr;
            }
        }
        done += chunk;
    }
    if (a->have_size && a->init_size < size) {   /* hinter der initialisierten Groesse: Nullen */
        uint64_t zs = a->init_size > off ? a->init_size - off : 0;
        if (zs < len) memset((uint8_t *)buf + zs, 0, (size_t)(len - zs));
    }
    return (int64_t)len;
}

static int read_record(Ntfs *n, uint64_t recno, uint8_t *buf) {
    int r;
    if (n->mft.nruns == 0) {   /* Start: Record 0 liegt direkt am MFT-Anfang */
        if (recno != 0) return FS_EIO;
        r = rd_bytes(n, n->mft_lcn * n->cbytes, buf, n->rec_size);
    } else {
        int64_t k = attr_read(n, &n->mft, recno * n->rec_size, buf, n->rec_size);
        r = (k == (int64_t)n->rec_size) ? 0 : (k < 0 ? (int)k : FS_EIO);
    }
    if (r) return r;
    r = apply_fixups(buf, n->rec_size, "FILE");
    if (r) return r;
    if (!(le16(buf + 0x16) & 1)) return FS_ENOENT;   /* Record nicht in Benutzung */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Namen                                                               */
/* ------------------------------------------------------------------ */
static uint16_t upc(Ntfs *n, uint16_t c) {
    if (n->upcase && c < n->upcase_n) return n->upcase[c];
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
}

/* Vergleich zweier UTF-16LE-Namen (Bytefolgen) wie die NTFS-Sortierung: Grossschreibung egal */
static int ucmp(Ntfs *n, const uint8_t *a, int alen, const uint8_t *b, int blen) {
    int m = alen < blen ? alen : blen;
    for (int i = 0; i < m; i++) {
        uint16_t x = upc(n, le16(a + 2 * i)), y = upc(n, le16(b + 2 * i));
        if (x != y) return x < y ? -1 : 1;
    }
    return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

static void utf16_to_ascii(const uint8_t *p, int len, char *out, size_t max) {
    size_t n = 0;
    for (int i = 0; i < len && n + 1 < max; i++) {
        uint16_t c = le16(p + 2 * i);
        out[n++] = c < 128 ? (char)c : '?';
    }
    out[n] = 0;
}

/* ------------------------------------------------------------------ */
/* Verzeichnisindex (B+-Baum)                                          */
/* ------------------------------------------------------------------ */
static void dir_close(Dir *d) {
    attr_free(&d->root);
    attr_free(&d->alloc);
}

static int dir_open(Ntfs *n, uint64_t recno, Dir *d) {
    memset(d, 0, sizeof *d);
    int r = attr_load(n, recno, AT_INDEX_ROOT, "$I30", &d->root);
    if (r) return r == FS_ENOENT ? FS_ENOTDIR : r;
    if (!d->root.resident || d->root.rlen < 0x20) { dir_close(d); return FS_EIO; }
    d->bsize = le32(d->root.rdata + 8);
    d->has_alloc = attr_load(n, recno, AT_INDEX_ALLOC, "$I30", &d->alloc) == 0;
    if (d->bsize < 512 || d->bsize > 65536) { dir_close(d); return FS_EIO; }
    return 0;
}

static uint8_t *load_index_block(Ntfs *n, Dir *d, uint64_t vcn) {
    if (!d->has_alloc) return 0;
    uint64_t unit = d->bsize >= n->cbytes ? n->cbytes : 512;
    uint8_t *b = kapi.malloc(d->bsize);
    if (!b) return 0;
    if (attr_read(n, &d->alloc, vcn * unit, b, d->bsize) != (int64_t)d->bsize || apply_fixups(b, d->bsize, "INDX")) {
        kapi.free(b);
        return 0;
    }
    return b;
}

typedef int (*EntryCb)(Ntfs *n, const uint8_t *entry, void *ctx);

/* in Reihenfolge durch den Baum laufen; cb != 0 beendet */
static int walk_entries(Ntfs *n, Dir *d, const uint8_t *ents, uint32_t avail, EntryCb cb, void *ctx, int depth) {
    if (depth > 16) return FS_EIO;
    uint32_t off = 0;
    while (off + 16 <= avail) {
        const uint8_t *e = ents + off;
        uint32_t elen = le16(e + 8), flags = le16(e + 12);
        if (elen < 16 || off + elen > avail) return FS_EIO;
        if (flags & 1) {
            uint8_t *blk = load_index_block(n, d, le64(e + elen - 8));
            if (!blk) return FS_EIO;
            uint32_t eo = le32(blk + 0x18), tot = le32(blk + 0x1C);
            int r = (eo <= tot && tot <= d->bsize - 0x18) ? walk_entries(n, d, blk + 0x18 + eo, tot - eo, cb, ctx, depth + 1) : FS_EIO;
            kapi.free(blk);
            if (r) return r;
        }
        if (flags & 2) break;
        int r = cb(n, e, ctx);
        if (r) return r;
        off += elen;
    }
    return 0;
}

static int walk_dir(Ntfs *n, Dir *d, EntryCb cb, void *ctx) {
    const uint8_t *rd = d->root.rdata;
    uint32_t eo = le32(rd + 0x10), il = le32(rd + 0x14);
    if (eo > il || 0x10 + il > d->root.rlen) return FS_EIO;
    return walk_entries(n, d, rd + 0x10 + eo, il - eo, cb, ctx, 0);
}

/* Suche entlang der Baumordnung. 0 = gefunden, 1 = nicht gefunden */
static int tree_search(Ntfs *n, Dir *d, const uint8_t *ents, uint32_t avail, const uint8_t *key, int klen, uint64_t *ref, int depth) {
    if (depth > 16) return FS_EIO;
    uint32_t off = 0;
    while (off + 16 <= avail) {
        const uint8_t *e = ents + off;
        uint32_t elen = le16(e + 8), flags = le16(e + 12);
        if (elen < 16 || off + elen > avail) return FS_EIO;
        int descend = 0;
        if (flags & 2) {
            descend = flags & 1;
            if (!descend) return 1;
        } else {
            const uint8_t *fn = e + 0x10;
            int c = ucmp(n, key, klen, fn + 0x42, fn[0x40]);
            if (c == 0) {
                *ref = le64(e) & REF_MASK;
                return 0;
            }
            if (c < 0) {
                if (!(flags & 1)) return 1;
                descend = 1;
            }
        }
        if (descend) {
            uint8_t *blk = load_index_block(n, d, le64(e + elen - 8));
            if (!blk) return FS_EIO;
            uint32_t eo = le32(blk + 0x18), tot = le32(blk + 0x1C);
            int r = (eo <= tot && tot <= d->bsize - 0x18) ? tree_search(n, d, blk + 0x18 + eo, tot - eo, key, klen, ref, depth + 1) : FS_EIO;
            kapi.free(blk);
            return r;
        }
        off += elen;
    }
    return 1;
}

typedef struct {
    const uint8_t *key;
    int klen;
    uint64_t ref;
} FindCtx;

static int find_cb(Ntfs *n, const uint8_t *e, void *ctx) {
    FindCtx *f = ctx;
    const uint8_t *fn = e + 0x10;
    if (ucmp(n, f->key, f->klen, fn + 0x42, fn[0x40]) == 0) {
        f->ref = le64(e) & REF_MASK;
        return 1;
    }
    return 0;
}

/* Name in einem Verzeichnis suchen: Baumsuche, danach als Absicherung ein linearer Durchlauf */
static int dir_lookup(Ntfs *n, uint64_t dirrec, const uint8_t *key, int klen, uint64_t *ref) {
    Dir d;
    int r = dir_open(n, dirrec, &d);
    if (r) return r;
    const uint8_t *rd = d.root.rdata;
    uint32_t eo = le32(rd + 0x10), il = le32(rd + 0x14);
    if (eo <= il && 0x10 + il <= d.root.rlen) {
        r = tree_search(n, &d, rd + 0x10 + eo, il - eo, key, klen, ref, 0);
        if (r == 0) { dir_close(&d); return 0; }
    }
    FindCtx f = {key, klen, 0};
    r = walk_dir(n, &d, find_cb, &f);
    dir_close(&d);
    if (r == 1) { *ref = f.ref; return 0; }
    return r < 0 ? r : FS_ENOENT;
}

/* ------------------------------------------------------------------ */
/* Pfade                                                               */
/* ------------------------------------------------------------------ */
static int next_comp(const char **p, char *comp) {
    while (**p == '/') (*p)++;
    if (!**p) return 0;
    int k = 0;
    while (**p && **p != '/') {
        if (k < 255) comp[k++] = **p;
        (*p)++;
    }
    comp[k] = 0;
    return 1;
}

/* Pfad -> MFT-Record und optionaler Stromname ("datei:strom") */
static int resolve(Ntfs *n, const char *path, uint64_t *rec, char *stream) {
    uint64_t cur = 5;   /* Wurzelverzeichnis */
    stream[0] = 0;
    const char *p = path;
    char comp[256];
    while (next_comp(&p, comp)) {
        char *colon = 0;
        for (char *c = comp; *c; c++)
            if (*c == ':') { colon = c; break; }
        const char *rest = p;
        char tmp[256];   /* next_comp kopiert bis zu 255 Zeichen */
        int last = !next_comp(&rest, tmp);
        if (colon) {
            if (!last) return FS_ENOENT;
            *colon = 0;
            scopy(stream, colon + 1, 64);
        }
        uint8_t key[512];
        int kl = 0;
        for (const char *c = comp; *c && kl < 255; c++) {
            if ((uint8_t)*c > 127) return FS_ENOENT;
            key[2 * kl] = (uint8_t)*c;
            key[2 * kl + 1] = 0;
            kl++;
        }
        uint64_t ref;
        int r = dir_lookup(n, cur, key, kl, &ref);
        if (r) return r;
        cur = ref;
    }
    *rec = cur;
    return 0;
}

static int rec_is_dir(Ntfs *n, uint64_t rec, int *is_dir) {
    uint8_t *b = kapi.malloc(n->rec_size);
    if (!b) return FS_ENOSPC;
    int r = read_record(n, rec, b);
    if (!r) *is_dir = (le16(b + 0x16) & 2) ? 1 : 0;
    kapi.free(b);
    return r;
}

/* ------------------------------------------------------------------ */
/* Fs-Operationen                                                      */
/* ------------------------------------------------------------------ */
static int n_stat(Fs *fs, const char *path, FsEntry *out) {
    Ntfs *n = fs->priv;
    uint64_t rec;
    char stream[64];
    int r = resolve(n, path, &rec, stream);
    if (r) return r;
    int is_dir;
    r = rec_is_dir(n, rec, &is_dir);
    if (r) return r;
    const char *base = path;
    for (const char *c = path; *c; c++)
        if (*c == '/') base = c + 1;
    scopy(out->name, base, sizeof out->name);
    out->is_dir = (uint8_t)(is_dir && !stream[0]);
    out->size = 0;
    if (!out->is_dir) {
        Attr a;
        r = attr_load(n, rec, AT_DATA, stream[0] ? stream : 0, &a);
        if (r == 0) {
            out->size = a.real_size;
            attr_free(&a);
        } else if (r != FS_ENOENT) {
            return r;
        } else if (stream[0]) {
            return FS_ENOENT;
        }
    }
    return 0;
}

typedef struct {
    FsDirCb cb;
    void *ctx;
} ListCtx;

static int list_cb(Ntfs *n, const uint8_t *e, void *ctx) {
    (void)n;
    ListCtx *l = ctx;
    const uint8_t *fn = e + 0x10;
    uint8_t nlen = fn[0x40], ns = fn[0x41];
    if (ns == 2) return 0;                                    /* reiner DOS-Name: Duplikat ueberspringen */
    if (nlen == 1 && le16(fn + 0x42) == '.') return 0;        /* "." im Wurzelverzeichnis */
    FsEntry fe;
    utf16_to_ascii(fn + 0x42, nlen, fe.name, sizeof fe.name);
    fe.is_dir = (le32(fn + 0x38) & 0x10000000u) ? 1 : 0;
    fe.size = fe.is_dir ? 0 : le64(fn + 0x30);
    return l->cb(&fe, l->ctx) ? 2 : 0;
}

static int n_readdir(Fs *fs, const char *path, FsDirCb cb, void *ctx) {
    Ntfs *n = fs->priv;
    uint64_t rec;
    char stream[64];
    int r = resolve(n, path, &rec, stream);
    if (r) return r;
    Dir d;
    r = dir_open(n, rec, &d);
    if (r) return r;
    ListCtx l = {cb, ctx};
    r = walk_dir(n, &d, list_cb, &l);
    dir_close(&d);
    return r < 0 ? r : 0;
}

static int64_t n_read(Fs *fs, const char *path, uint64_t off, void *buf, uint64_t len) {
    Ntfs *n = fs->priv;
    uint64_t rec;
    char stream[64];
    int r = resolve(n, path, &rec, stream);
    if (r) return r;
    int is_dir;
    r = rec_is_dir(n, rec, &is_dir);
    if (r) return r;
    if (is_dir && !stream[0]) return FS_EISDIR;
    Attr a;
    r = attr_load(n, rec, AT_DATA, stream[0] ? stream : 0, &a);
    if (r) return r;
    int64_t k = attr_read(n, &a, off, buf, len);
    attr_free(&a);
    return k;
}

static int n_statfs(Fs *fs, uint64_t *total, uint64_t *free_b) {
    Ntfs *n = fs->priv;
    *total = n->clusters * n->cbytes;
    *free_b = 0;
    Attr bm;
    if (attr_load(n, 6, AT_DATA, 0, &bm)) return 0;   /* $Bitmap */
    uint8_t *buf = kapi.malloc(65536);
    if (buf) {
        uint64_t used = 0, bytes = (n->clusters + 7) / 8;
        for (uint64_t off = 0; off < bytes; off += 65536) {
            uint64_t c = bytes - off < 65536 ? bytes - off : 65536;
            if (attr_read(n, &bm, off, buf, c) != (int64_t)c) { used = n->clusters; break; }
            for (uint64_t i = 0; i < c; i++) {
                uint8_t v = buf[i];
                while (v) { used += v & 1; v >>= 1; }
            }
        }
        if (used > n->clusters) used = n->clusters;
        *free_b = (n->clusters - used) * n->cbytes;
        kapi.free(buf);
    }
    attr_free(&bm);
    return 0;
}

/* ================================================================== */
/* Schreiben                                                           */
/* ================================================================== */
/*
 * Grenzen des Schreibteils (bewusst klein gehalten):
 *   - nur Records OHNE $ATTRIBUTE_LIST (Dateien mit vielen Fragmenten/Namen/Streams: "nicht unterstuetzt")
 *   - keine Kompression, keine Verschluesselung, keine ACLs (neue Dateien erben die Security-ID des Verzeichnisses)
 *   - Verzeichnisindizes werden bei jeder Aenderung komplett neu aufgebaut (O(n) pro Aenderung)
 *   - das Journal ($LogFile) wird nicht geschrieben: Beim ersten Schreiben wird es wie bei ntfs-3g
 *     zurueckgesetzt, waehrend einer Aenderung ist das Dirty-Flag des Volumes gesetzt
 *   - Schreiben nur, wenn das Volume sauber ist (kein Dirty-Flag, sauberes Journal, kein Ruhezustand)
 */
static inline void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void p32(uint8_t *p, uint32_t v) { p16(p, (uint16_t)v); p16(p + 2, (uint16_t)(v >> 16)); }
static inline void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }
#define ALIGN8(x) (((x) + 7u) & ~7u)

#define AT_BITMAP 0xB0

static int wr_bytes(Ntfs *n, uint64_t off, const void *buf, uint64_t len) {
    uint32_t ss = n->dev->sector_size;
    const uint8_t *in = buf;
    uint8_t *tmp = 0;
    int r = 0;
    uint32_t head = (uint32_t)(off % ss);
    if (head || (len % ss)) {
        tmp = kapi.malloc(ss);
        if (!tmp) return FS_ENOSPC;
    }
    if (head && len) {
        if (n->dev->read(n->dev, off / ss, 1, tmp)) { r = FS_EIO; goto done; }
        uint64_t c = ss - head;
        if (c > len) c = len;
        memcpy(tmp + head, in, (size_t)c);
        if (n->dev->write(n->dev, off / ss, 1, tmp)) { r = FS_EIO; goto done; }
        off += c; in += c; len -= c;
    }
    uint64_t mid = len - len % ss;
    if (mid) {
        if (n->dev->write(n->dev, off / ss, (uint32_t)(mid / ss), in)) { r = FS_EIO; goto done; }
        off += mid; in += mid; len -= mid;
    }
    if (len) {
        if (n->dev->read(n->dev, off / ss, 1, tmp)) { r = FS_EIO; goto done; }
        memcpy(tmp, in, (size_t)len);
        if (n->dev->write(n->dev, off / ss, 1, tmp)) r = FS_EIO;
    }
done:
    kapi.free(tmp);
    return r;
}

/* in bereits zugeordnete Cluster eines nicht residenten Attributs schreiben */
static int attr_write(Ntfs *n, Attr *a, uint64_t off, const void *buf, uint64_t len) {
    const uint8_t *in = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done, vcn = pos / n->cbytes, inn = pos % n->cbytes;
        const Run *r = 0;
        for (uint32_t i = 0; i < a->nruns; i++)
            if (vcn >= a->runs[i].vcn && vcn < a->runs[i].vcn + a->runs[i].len) { r = &a->runs[i]; break; }
        if (!r || r->lcn == SPARSE_LCN) return FS_EIO;
        uint64_t avail = (r->vcn + r->len - vcn) * n->cbytes - inn;
        uint64_t chunk = avail < len - done ? avail : len - done;
        int rr = wr_bytes(n, (r->lcn + (vcn - r->vcn)) * n->cbytes + inn, in + done, chunk);
        if (rr) return rr;
        done += chunk;
    }
    return 0;
}

/* wie read_record, aber ohne Pruefung "in Benutzung" (fuer freie Records) */
static int read_record_raw(Ntfs *n, uint64_t recno, uint8_t *buf) {
    int64_t k = attr_read(n, &n->mft, recno * n->rec_size, buf, n->rec_size);
    if (k != (int64_t)n->rec_size) return k < 0 ? (int)k : FS_EIO;
    if (!memeq(buf, "FILE", 4)) return FS_ENOENT;
    return apply_fixups(buf, n->rec_size, "FILE");
}

/* Record (logische Sicht, ohne Fixups) auf den Datentraeger schreiben */
static int write_record(Ntfs *n, uint64_t recno, const uint8_t *rec) {
    uint8_t *t = kapi.malloc(n->rec_size);
    if (!t) return FS_ENOSPC;
    memcpy(t, rec, n->rec_size);
    uint32_t uo = le16(t + 4), uc = le16(t + 6);
    uint16_t usn = (uint16_t)(le16(t + uo) + 1);
    if (usn == 0 || usn == 0xFFFF) usn = 1;
    p16(t + uo, usn);
    for (uint32_t i = 1; i < uc; i++) {
        uint32_t pos = i * 512 - 2;
        t[uo + 2 * i] = t[pos];
        t[uo + 2 * i + 1] = t[pos + 1];
        p16(t + pos, usn);
    }
    int r = attr_write(n, &n->mft, recno * n->rec_size, t, n->rec_size);
    if (!r && recno < 4 && n->mftmirr_lcn)
        r = wr_bytes(n, n->mftmirr_lcn * n->cbytes + recno * n->rec_size, t, n->rec_size);
    kapi.free(t);
    return r;
}

/* ---- Zeit (CMOS-Uhr -> NTFS-Zeit: 100-ns-Schritte seit 1601) ---- */
static uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }

static uint64_t now_ntfs(void) {
    for (int i = 0; i < 1000 && (cmos(0x0A) & 0x80); i++) {
    }
    int64_t s = cmos(0), mi = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9);
    if (!(cmos(0x0B) & 4)) {
        s = (s & 15) + (s >> 4) * 10; mi = (mi & 15) + (mi >> 4) * 10; h = (h & 15) + ((h >> 4) & 7) * 10;
        d = (d & 15) + (d >> 4) * 10; mo = (mo & 15) + (mo >> 4) * 10; y = (y & 15) + (y >> 4) * 10;
    }
    int64_t year = 2000 + y;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) { year = 2026; mo = 1; d = 1; h = mi = s = 0; }
    int64_t yy = year - (mo <= 2);
    int64_t era = yy / 400, yoe = yy - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;   /* Tage seit 1970-01-01 */
    int64_t unix_s = days * 86400 + h * 3600 + mi * 60 + s;
    return ((uint64_t)unix_s + 11644473600ULL) * 10000000ULL;
}

/* ------------------------------------------------------------------ */
/* Record bearbeiten (Record liegt im Speicher, ohne Fixups)           */
/* ------------------------------------------------------------------ */
static uint32_t rec_find(const uint8_t *rec, uint32_t type, const char *name) {
    uint32_t used = le32(rec + 0x18), off = le16(rec + 0x14);
    while (off + 16 <= used) {
        uint32_t t = le32(rec + off);
        if (t == AT_END) break;
        uint32_t l = le32(rec + off + 4);
        if (l < 16 || off + l > used) break;
        if (t == type && attr_name_eq(rec + off, name)) return off;
        off += l;
    }
    return 0;
}

/* naechstes Attribut nach off (0 = Ende) */
static uint32_t rec_next(const uint8_t *rec, uint32_t off) {
    uint32_t used = le32(rec + 0x18);
    uint32_t l = le32(rec + off + 4);
    if (l < 16) return 0;
    off += l;
    if (off + 16 > used || le32(rec + off) == AT_END) return 0;
    return off;
}

static uint32_t rec_first(const uint8_t *rec) {
    uint32_t off = le16(rec + 0x14), used = le32(rec + 0x18);
    if (off + 16 > used || le32(rec + off) == AT_END) return 0;
    return off;
}

static void rec_remove_attr(uint8_t *rec, uint32_t off) {
    uint32_t used = le32(rec + 0x18), l = le32(rec + off + 4);
    memmove(rec + off, rec + off + l, used - off - l);
    memset(rec + used - l, 0, l);
    p32(rec + 0x18, used - l);
}

/* Attribut mit gleichem Typ/Namen ersetzen oder in Typ-Reihenfolge einfuegen */
static int rec_put_attr(uint8_t *rec, uint32_t type, const char *name, const uint8_t *attr, uint32_t len) {
    uint32_t used = le32(rec + 0x18), alloc = le32(rec + 0x1C);
    uint32_t off = rec_find(rec, type, name);
    uint32_t oldlen = off ? le32(rec + off + 4) : 0;
    if (used - oldlen + len > alloc) return FS_ENOSPC;
    uint16_t inst;
    if (off) {
        inst = le16(rec + off + 0xE);
        memmove(rec + off + len, rec + off + oldlen, used - off - oldlen);
        if (len < oldlen) memset(rec + used - oldlen + len, 0, oldlen - len);
    } else {
        uint32_t pos = le16(rec + 0x14);
        while (pos + 16 <= used) {
            uint32_t t = le32(rec + pos);
            if (t == AT_END || t > type) break;
            pos += le32(rec + pos + 4);
        }
        off = pos;
        inst = le16(rec + 0x28);
        p16(rec + 0x28, (uint16_t)(inst + 1));
        memmove(rec + off + len, rec + off, used - off);
    }
    memcpy(rec + off, attr, len);
    p16(rec + off + 0xE, inst);
    p32(rec + 0x18, used - oldlen + len);
    return 0;
}

/* residentes Attribut bauen; Rueckgabe: Laenge */
static uint32_t mk_res(uint8_t *a, uint32_t type, const char *name, const uint8_t *val, uint32_t vlen, uint8_t indexed) {
    uint32_t nl = name ? (uint32_t)slen(name) : 0;
    uint32_t voff = ALIGN8(0x18 + nl * 2), len = ALIGN8(voff + vlen);
    memset(a, 0, len);
    p32(a, type);
    p32(a + 4, len);
    a[9] = (uint8_t)nl;
    p16(a + 10, 0x18);
    p32(a + 0x10, vlen);
    p16(a + 0x14, (uint16_t)voff);
    a[0x16] = indexed;
    for (uint32_t i = 0; i < nl; i++) p16(a + 0x18 + 2 * i, (uint16_t)(uint8_t)name[i]);
    if (vlen && val) memcpy(a + voff, val, vlen);
    return len;
}

static int sbytes(int64_t v) {
    for (int b = 1; b < 8; b++) {
        int64_t lim = (int64_t)1 << (8 * b - 1);
        if (v >= -lim && v < lim) return b;
    }
    return 8;
}

/* Runliste kodieren (inkl. Abschluss-0); Rueckgabe: Laenge oder -1 */
static int encode_runs(const Run *runs, uint32_t nruns, uint8_t *out, uint32_t max) {
    int64_t prev = 0;
    uint32_t p = 0;
    for (uint32_t i = 0; i < nruns; i++) {
        int lb = sbytes((int64_t)runs[i].len);   /* Laengen gelten als vorzeichenbehaftet: 171 braucht 2 Bytes (AB 00) */
        int ob = 0;
        int64_t delta = 0;
        if (runs[i].lcn != SPARSE_LCN) {
            delta = (int64_t)runs[i].lcn - prev;
            ob = sbytes(delta);
        }
        if (p + 1 + (uint32_t)lb + (uint32_t)ob + 1 > max) return -1;
        out[p++] = (uint8_t)(lb | (ob << 4));
        for (int k = 0; k < lb; k++) out[p++] = (uint8_t)(runs[i].len >> (8 * k));
        if (ob) {
            for (int k = 0; k < ob; k++) out[p++] = (uint8_t)((uint64_t)delta >> (8 * k));
            prev = (int64_t)runs[i].lcn;
        }
    }
    out[p++] = 0;
    return (int)p;
}

/* nicht residentes Attribut bauen; Rueckgabe: Laenge oder -1 */
static int mk_nonres(uint8_t *a, uint32_t cap, uint32_t type, const char *name, const Run *runs, uint32_t nruns,
                     uint64_t alloc, uint64_t real, uint64_t init, uint32_t cbytes) {
    uint32_t nl = name ? (uint32_t)slen(name) : 0;
    uint32_t rl = ALIGN8(0x40 + nl * 2);
    if (cap < rl + 8) return -1;
    memset(a, 0, rl);
    int el = encode_runs(runs, nruns, a + rl, cap - rl);
    if (el < 0) return -1;
    uint32_t len = ALIGN8(rl + (uint32_t)el);
    if (len > cap) return -1;
    memset(a + rl + el, 0, len - rl - (uint32_t)el);
    p32(a, type);
    p32(a + 4, len);
    a[8] = 1;
    a[9] = (uint8_t)nl;
    p16(a + 10, 0x40);
    p64(a + 0x10, 0);
    p64(a + 0x18, alloc ? alloc / cbytes - 1 : (uint64_t)-1);
    p16(a + 0x20, (uint16_t)rl);
    p64(a + 0x28, alloc);
    p64(a + 0x30, real);
    p64(a + 0x38, init);
    for (uint32_t i = 0; i < nl; i++) p16(a + 0x40 + 2 * i, (uint16_t)(uint8_t)name[i]);
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* Cluster-Bitmap und MFT-Verwaltung                                   */
/* ------------------------------------------------------------------ */
static int bit_get(const uint8_t *b, uint64_t i) { return (b[i >> 3] >> (i & 7)) & 1; }
static void bit_put(uint8_t *b, uint64_t i, int v) {
    if (v) b[i >> 3] = (uint8_t)(b[i >> 3] | (1u << (i & 7)));
    else b[i >> 3] = (uint8_t)(b[i >> 3] & ~(1u << (i & 7)));
}

static int cl_mark(Ntfs *n, uint64_t lcn, uint64_t len, int v) {
    for (uint64_t i = 0; i < len; i++) bit_put(n->bm, lcn + i, v);
    return attr_write(n, &n->bm_attr, lcn >> 3, n->bm + (lcn >> 3), ((lcn + len - 1) >> 3) - (lcn >> 3) + 1);
}

static int next_free_run(Ntfs *n, uint64_t pos, uint64_t limit, uint64_t *start, uint64_t *len) {
    while (pos < limit) {
        if ((pos & 7) == 0 && pos + 8 <= limit && n->bm[pos >> 3] == 0xFF) { pos += 8; continue; }
        if (!bit_get(n->bm, pos)) {
            uint64_t e = pos + 1;
            while (e < limit && !bit_get(n->bm, e)) e++;
            *start = pos;
            *len = e - pos;
            return 1;
        }
        pos++;
    }
    return 0;
}

/* count Cluster belegen (moeglichst am Stueck, ab 'goal'); Laufliste in runs (vcn noch 0) */
static int cl_alloc(Ntfs *n, uint64_t count, uint64_t goal, Run *runs, uint32_t *nruns, uint32_t maxruns) {
    uint64_t s, l;
    *nruns = 0;
    if (goal >= n->clusters) goal = 0;
    for (int pass = 0; pass < 2; pass++) {   /* 1. ein zusammenhaengender Block, 2. in Stuecken */
        uint64_t remaining = count, pos = goal;
        int wrapped = 0;
        *nruns = 0;
        for (;;) {
            uint64_t lim = wrapped ? goal : n->clusters;
            if (!next_free_run(n, pos, lim, &s, &l)) {
                if (wrapped || goal == 0) break;
                wrapped = 1;
                pos = 0;
                continue;
            }
            if (pass == 0) {
                if (l >= count) {
                    runs[0].vcn = 0; runs[0].len = count; runs[0].lcn = s;
                    *nruns = 1;
                    remaining = 0;
                    break;
                }
                pos = s + l;
                continue;
            }
            uint64_t take = l < remaining ? l : remaining;
            if (*nruns >= maxruns) return FS_ENOSPC;
            runs[*nruns].vcn = 0; runs[*nruns].len = take; runs[*nruns].lcn = s;
            (*nruns)++;
            remaining -= take;
            pos = s + take;
            if (!remaining) break;
        }
        if (remaining == 0) {
            for (uint32_t i = 0; i < *nruns; i++) {
                int r = cl_mark(n, runs[i].lcn, runs[i].len, 1);
                if (r) return r;
            }
            n->alloc_hint = runs[*nruns - 1].lcn + runs[*nruns - 1].len;
            return 0;
        }
    }
    return FS_ENOSPC;
}

static int cl_free(Ntfs *n, uint64_t lcn, uint64_t len) {
    if (lcn + len > n->clusters) return FS_EIO;
    return cl_mark(n, lcn, len, 0);
}

static int zero_clusters(Ntfs *n, uint64_t lcn, uint64_t len) {
    uint64_t chunk = len < 16 ? len : 16;
    uint8_t *z = kapi.malloc((size_t)(chunk * n->cbytes));
    if (!z) return FS_ENOSPC;
    memset(z, 0, (size_t)(chunk * n->cbytes));
    int r = 0;
    for (uint64_t done = 0; done < len && !r; done += chunk) {
        uint64_t c = len - done < chunk ? len - done : chunk;
        r = wr_bytes(n, (lcn + done) * n->cbytes, z, c * n->cbytes);
    }
    kapi.free(z);
    return r;
}

/* alle Cluster eines nicht residenten Attributs (Rohbytes im Record) freigeben */
static int attr_free_clusters(Ntfs *n, const uint8_t *a) {
    if (!a[8]) return 0;
    Attr t;
    memset(&t, 0, sizeof t);
    int r = process_attr(&t, a);
    for (uint32_t i = 0; !r && i < t.nruns; i++)
        if (t.runs[i].lcn != SPARSE_LCN) r = cl_free(n, t.runs[i].lcn, t.runs[i].len);
    attr_free(&t);
    return r;
}

/* neue Cluster an ein Attribut anhaengen (verschmilzt mit dem letzten Run); neue Runs fuer Rollback in nr */
static int attr_grow(Ntfs *n, Attr *a, uint64_t count, Run *nr, uint32_t *nnr) {
    uint64_t goal = n->alloc_hint, vend = 0;
    if (a->nruns) {
        const Run *l = &a->runs[a->nruns - 1];
        vend = l->vcn + l->len;
        if (l->lcn != SPARSE_LCN) goal = l->lcn + l->len;
    }
    int r = cl_alloc(n, count, goal, nr, nnr, 32);
    if (r) return r;
    for (uint32_t i = 0; i < *nnr; i++) {
        Run *l = a->nruns ? &a->runs[a->nruns - 1] : 0;
        if (l && l->lcn != SPARSE_LCN && l->lcn + l->len == nr[i].lcn) l->len += nr[i].len;
        else {
            r = run_add(a, vend, nr[i].len, nr[i].lcn);
            if (r) return r;
        }
        vend += nr[i].len;
    }
    return 0;
}

static uint64_t attr_clusters(const Attr *a) {
    uint64_t c = 0;
    for (uint32_t i = 0; i < a->nruns; i++) c += a->runs[i].len;
    return c;
}

static void rollback_new(Ntfs *n, const Run *nr, uint32_t nnr) {
    for (uint32_t i = 0; i < nnr; i++) cl_free(n, nr[i].lcn, nr[i].len);
}

/* $MFT um Records erweitern (Record 0 und die MFT-Bitmap werden angepasst) */
static int mft_extend(Ntfs *n) {
    uint64_t add_cl = ((uint64_t)32 * n->rec_size + n->cbytes - 1) / n->cbytes;
    Run nr[32];
    uint32_t nnr;
    int r = attr_grow(n, &n->mft, add_cl, nr, &nnr);
    if (r) return r;
    for (uint32_t i = 0; i < nnr; i++) {
        r = zero_clusters(n, nr[i].lcn, nr[i].len);
        if (r) return r;
    }
    uint64_t total = attr_clusters(&n->mft) * n->cbytes;
    n->mft.real_size = total;
    n->mft.init_size = total;
    n->mft.have_size = 1;

    uint8_t *rec0 = kapi.malloc(n->rec_size);
    uint8_t *tmp = kapi.malloc(n->rec_size);
    if (!rec0 || !tmp) { kapi.free(rec0); kapi.free(tmp); return FS_ENOSPC; }
    r = read_record(n, 0, rec0);
    if (!r) {
        int el = mk_nonres(tmp, n->rec_size, AT_DATA, 0, n->mft.runs, n->mft.nruns, total, total, total, n->cbytes);
        r = el < 0 ? FS_ENOSPC : rec_put_attr(rec0, AT_DATA, 0, tmp, (uint32_t)el);
    }
    if (!r) {
        uint64_t recs = total / n->rec_size;
        uint32_t need = ALIGN8((uint32_t)((recs + 7) / 8));
        uint32_t boff = rec_find(rec0, AT_BITMAP, 0);
        if (!boff || !rec0[boff + 8] || le64(rec0 + boff + 0x28) < need) r = FS_ENOSPC;
        else if (need > n->mbm_bytes) {
            uint8_t *nb = kapi.realloc(n->mbm, need);
            if (!nb) r = FS_ENOSPC;
            else {
                memset(nb + n->mbm_bytes, 0, need - n->mbm_bytes);
                n->mbm = nb;
                p64(rec0 + boff + 0x30, need);
                p64(rec0 + boff + 0x38, need);
                n->mbm_attr.real_size = need;
                n->mbm_attr.init_size = need;
                r = attr_write(n, &n->mbm_attr, n->mbm_bytes, nb + n->mbm_bytes, need - n->mbm_bytes);
                n->mbm_bytes = need;
            }
        }
        if (!r) {
            n->mft_records = recs;
            r = write_record(n, 0, rec0);
        }
    }
    kapi.free(rec0);
    kapi.free(tmp);
    return r;
}

static int mrec_alloc(Ntfs *n, uint64_t *recno) {
    for (int tries = 0; tries < 4; tries++) {
        for (uint64_t i = 24; i < n->mft_records && (i >> 3) < n->mbm_bytes; i++) {
            if (!bit_get(n->mbm, i)) {
                bit_put(n->mbm, i, 1);
                int r = attr_write(n, &n->mbm_attr, i >> 3, n->mbm + (i >> 3), 1);
                if (r) return r;
                *recno = i;
                return 0;
            }
        }
        int r = mft_extend(n);
        if (r) return r;
    }
    return FS_ENOSPC;
}

static int mrec_free(Ntfs *n, uint64_t recno) {
    bit_put(n->mbm, recno, 0);
    return attr_write(n, &n->mbm_attr, recno >> 3, n->mbm + (recno >> 3), 1);
}

/* ------------------------------------------------------------------ */
/* Volume pruefen und vorbereiten                                      */
/* ------------------------------------------------------------------ */
static int vol_flags(Ntfs *n, int *dirty) {
    uint8_t *rec = kapi.malloc(n->rec_size);
    if (!rec) return FS_ENOSPC;
    int r = read_record(n, 3, rec);
    if (!r) {
        uint32_t off = rec_find(rec, 0x70, 0);   /* $VOLUME_INFORMATION */
        if (!off || rec[off + 8]) r = FS_EIO;
        else *dirty = le16(rec + off + le16(rec + off + 0x14) + 10) & 1;
    }
    kapi.free(rec);
    return r;
}

static int vol_set_dirty(Ntfs *n, int on) {
    uint8_t *rec = kapi.malloc(n->rec_size);
    if (!rec) return FS_ENOSPC;
    int r = read_record(n, 3, rec);
    if (!r) {
        uint32_t off = rec_find(rec, 0x70, 0);
        if (!off || rec[off + 8]) r = FS_EIO;
        else {
            uint8_t *f = rec + off + le16(rec + off + 0x14) + 10;
            uint16_t nf = on ? (uint16_t)(le16(f) | 1) : (uint16_t)(le16(f) & ~1);
            if (nf != le16(f)) {
                p16(f, nf);
                r = write_record(n, 3, rec);
            }
        }
    }
    kapi.free(rec);
    return r;
}

/* Journal sauber (oder leer)? 0 = ja */
static int logfile_clean(Ntfs *n) {
    Attr lf;
    int r = attr_load(n, 2, AT_DATA, 0, &lf);
    if (r) return r;
    uint8_t *pg = kapi.malloc(4096);
    r = FS_EDIRTY;
    if (pg && attr_read(n, &lf, 0, pg, 4096) == 4096) {
        if (le32(pg) == 0xFFFFFFFFu) r = 0;   /* leer (frisch formatiert / zurueckgesetzt) */
        else if (memeq(pg, "RSTR", 4) && apply_fixups(pg, 4096, "RSTR") == 0) {
            uint32_t ra = le16(pg + 0x18);
            if (ra + 0x10 <= 4096 && (le16(pg + ra + 0xE) & 2)) r = 0;   /* RESTART_VOLUME_IS_CLEAN */
        }
    }
    kapi.free(pg);
    attr_free(&lf);
    return r;
}

static int logfile_reset(Ntfs *n) {
    Attr lf;
    int r = attr_load(n, 2, AT_DATA, 0, &lf);
    if (r) return r;
    uint32_t chunk = 65536;
    uint8_t *ff = kapi.malloc(chunk);
    if (!ff) { attr_free(&lf); return FS_ENOSPC; }
    memset(ff, 0xFF, chunk);
    for (uint64_t off = 0; !r && off < lf.real_size; off += chunk) {
        uint64_t c = lf.real_size - off < chunk ? lf.real_size - off : chunk;
        r = attr_write(n, &lf, off, ff, c);
    }
    kapi.free(ff);
    attr_free(&lf);
    return r;
}

static int resolve(Ntfs *n, const char *path, uint64_t *rec, char *stream);

/* prueft, ob Schreiben sicher ist (Dirty-Flag, Journal, Ruhezustand) */
static int n_enable_write(Fs *fs) {
    Ntfs *n = fs->priv;
    int dirty = 0;
    int r = vol_flags(n, &dirty);
    if (r) return r;
    if (dirty) return FS_EDIRTY;
    r = logfile_clean(n);
    if (r) return r;
    uint64_t rec;
    char st[64];
    if (resolve(n, "/hiberfil.sys", &rec, st) == 0) {   /* Windows-Ruhezustand/Schnellstart? */
        uint8_t h[4];
        Attr a;
        if (attr_load(n, rec, AT_DATA, 0, &a) == 0) {
            if (attr_read(n, &a, 0, h, 4) == 4 && (memeq(h, "hibr", 4) || memeq(h, "HIBR", 4))) {
                attr_free(&a);
                return FS_EDIRTY;
            }
            attr_free(&a);
        }
    }
    return 0;
}

/* beim ersten Schreiben: Bitmaps laden, Journal zuruecksetzen */
static int wprep(Ntfs *n) {
    if (n->wready) return 0;
    int r = attr_load(n, 6, AT_DATA, 0, &n->bm_attr);
    if (r) return r;
    n->bm_bytes = n->bm_attr.real_size;
    if (n->bm_bytes < (n->clusters + 7) / 8) return FS_EIO;
    n->bm = kapi.malloc((size_t)n->bm_bytes);
    if (!n->bm || attr_read(n, &n->bm_attr, 0, n->bm, n->bm_bytes) != (int64_t)n->bm_bytes) return FS_EIO;
    r = attr_load(n, 0, AT_BITMAP, 0, &n->mbm_attr);
    if (r) return r;
    n->mbm_bytes = n->mbm_attr.real_size;
    n->mbm = kapi.malloc((size_t)n->mbm_bytes + 8);
    if (!n->mbm || attr_read(n, &n->mbm_attr, 0, n->mbm, n->mbm_bytes) != (int64_t)n->mbm_bytes) return FS_EIO;
    n->mft_records = n->mft.real_size / n->rec_size;
    r = logfile_reset(n);
    if (r) return r;
    n->wready = 1;
    return 0;
}

static int begin_write(Ntfs *n) {
    int r = wprep(n);
    if (r) return r;
    return vol_set_dirty(n, 1);
}

static int end_write(Ntfs *n) {
    return vol_set_dirty(n, 0);
}

/* ------------------------------------------------------------------ */
/* Dateidaten schreiben                                                */
/* ------------------------------------------------------------------ */
/* Datenstrom (unbenannt oder benannt) im Record ersetzen oder anhaengen.
 * Residente Daten bleiben resident, solange sie in den Record passen; sonst werden Cluster belegt.
 * Schlaegt etwas fehl, bleiben Record und Cluster-Bitmap unveraendert (neue Cluster werden freigegeben). */
static int stream_set(Ntfs *n, uint8_t *rec, const char *stream, const uint8_t *data, uint64_t len, int append) {
    if (rec_find(rec, AT_ATTR_LIST, 0)) return FS_ENOSYS;
    int r = 0;
    uint32_t nl = stream ? (uint32_t)slen(stream) : 0;
    uint32_t off = rec_find(rec, AT_DATA, stream);
    if (!off) {   /* Strom neu anlegen (leer, resident) */
        uint8_t tmp[256];
        uint32_t l = mk_res(tmp, AT_DATA, stream, 0, 0, 0);
        r = rec_put_attr(rec, AT_DATA, stream, tmp, l);
        if (r) return r;
        off = rec_find(rec, AT_DATA, stream);
    }
    const uint8_t *a = rec + off;
    int old_res = !a[8];
    uint64_t old_size;
    Attr old;
    memset(&old, 0, sizeof old);
    if (old_res) {
        old_size = le32(a + 0x10);
    } else {
        uint16_t fl = le16(a + 12);
        if (fl & 0x4001) return FS_ENOSYS;                 /* komprimiert oder verschluesselt */
        if ((fl & 0x8000) && append) return FS_ENOSYS;     /* Sparse-Datei anhaengen */
        r = process_attr(&old, a);
        if (r) { attr_free(&old); return r; }
        old_size = old.real_size;
    }
    uint64_t new_total = append ? old_size + len : len;
    uint32_t used = le32(rec + 0x18), alloc = le32(rec + 0x1C), oldattr = le32(a + 4);
    uint32_t voff = ALIGN8(0x18 + nl * 2);
    int fits_res = new_total <= 60000 && (uint64_t)used - oldattr + ALIGN8(voff + (uint32_t)new_total) <= alloc;

    if (fits_res) {
        uint8_t *buf = kapi.malloc((size_t)voff + (size_t)new_total + 16);
        uint8_t *full = kapi.malloc((size_t)new_total + 1);
        if (!buf || !full) r = FS_ENOSPC;
        if (!r) {
            uint64_t pos = 0;
            if (append && old_size) {
                if (old_res) memcpy(full, a + le16(a + 0x14), (size_t)old_size);
                else if (attr_read(n, &old, 0, full, old_size) != (int64_t)old_size) r = FS_EIO;
                pos = old_size;
            }
            if (!r) {
                if (len) memcpy(full + pos, data, (size_t)len);
                uint32_t l = mk_res(buf, AT_DATA, stream, full, (uint32_t)new_total, 0);
                r = rec_put_attr(rec, AT_DATA, stream, buf, l);
                for (uint32_t i = 0; !r && !old_res && i < old.nruns; i++)
                    if (old.runs[i].lcn != SPARSE_LCN) r = cl_free(n, old.runs[i].lcn, old.runs[i].len);
            }
        }
        kapi.free(buf);
        kapi.free(full);
        attr_free(&old);
        return r;
    }

    /* nicht resident */
    uint64_t need_cl = (new_total + n->cbytes - 1) / n->cbytes;
    Attr na;
    memset(&na, 0, sizeof na);
    Run nr[32];
    uint32_t nnr = 0;
    uint8_t *oldres_copy = 0;
    uint8_t *abuf = 0;
    int keep_old = !old_res && append;
    if (keep_old) {
        for (uint32_t i = 0; !r && i < old.nruns; i++) r = run_add(&na, old.runs[i].vcn, old.runs[i].len, old.runs[i].lcn);
        uint64_t have = attr_clusters(&na);
        if (!r && need_cl > have) r = attr_grow(n, &na, need_cl - have, nr, &nnr);
    } else {
        r = attr_grow(n, &na, need_cl, nr, &nnr);
    }
    if (!r) {
        if (keep_old) {
            if (len) r = attr_write(n, &na, old_size, data, len);
        } else {
            if (old_res && append && old_size) {
                oldres_copy = kapi.malloc((size_t)old_size);
                if (!oldres_copy) r = FS_ENOSPC;
                else {
                    memcpy(oldres_copy, a + le16(a + 0x14), (size_t)old_size);
                    r = attr_write(n, &na, 0, oldres_copy, old_size);
                }
            }
            if (!r && len) r = attr_write(n, &na, append ? old_size : 0, data, len);
        }
    }
    if (!r) {
        uint32_t cap = 0x100 + nl * 2 + na.nruns * 18;
        abuf = kapi.malloc(cap);
        if (!abuf) r = FS_ENOSPC;
        else {
            int el = mk_nonres(abuf, cap, AT_DATA, stream, na.runs, na.nruns, need_cl * n->cbytes, new_total, new_total, n->cbytes);
            r = el < 0 ? FS_ENOSPC : rec_put_attr(rec, AT_DATA, stream, abuf, (uint32_t)el);
        }
    }
    if (r) rollback_new(n, nr, nnr);
    else if (!old_res && !append) {   /* ersetzen: alte Cluster erst jetzt freigeben */
        for (uint32_t i = 0; !r && i < old.nruns; i++)
            if (old.runs[i].lcn != SPARSE_LCN) r = cl_free(n, old.runs[i].lcn, old.runs[i].len);
    }
    kapi.free(oldres_copy);
    kapi.free(abuf);
    attr_free(&na);
    attr_free(&old);
    return r;
}

/* ------------------------------------------------------------------ */
/* Verzeichnisindex neu aufbauen                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *e;       /* Eintrag ohne Unterknoten-Zeiger: 16 + ausgerichteter Schluessel */
    uint32_t len;
    uint64_t child;   /* nur beim Aufbau: VCN des Unterknotens */
} IEnt;

typedef struct {
    IEnt *v;
    uint32_t n, cap;
} IList;

static int il_insert(IList *l, uint32_t pos, uint8_t *e, uint32_t len) {
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 16;
        IEnt *v = kapi.realloc(l->v, nc * sizeof(IEnt));
        if (!v) return FS_ENOSPC;
        l->v = v;
        l->cap = nc;
    }
    memmove(&l->v[pos + 1], &l->v[pos], (l->n - pos) * sizeof(IEnt));
    l->v[pos].e = e;
    l->v[pos].len = len;
    l->v[pos].child = 0;
    l->n++;
    return 0;
}

static void il_free(IList *l) {
    for (uint32_t i = 0; i < l->n; i++) kapi.free(l->v[i].e);
    kapi.free(l->v);
    memset(l, 0, sizeof *l);
}

static int collect_cb(Ntfs *n, const uint8_t *e, void *ctx) {
    (void)n;
    IList *l = ctx;
    uint32_t kl = le16(e + 10), len = 16 + ALIGN8(kl);
    if (len > le16(e + 8)) return FS_EIO;
    uint8_t *c = kapi.malloc(len);
    if (!c) return FS_ENOSPC;
    memcpy(c, e, len);
    p16(c + 8, (uint16_t)len);
    p16(c + 12, 0);
    int r = il_insert(l, l->n, c, len);
    if (r) kapi.free(c);
    return r;
}

static void indx_protect(uint8_t *img, uint32_t bsize) {
    uint32_t cnt = bsize / 512 + 1;
    memcpy(img, "INDX", 4);
    p16(img + 4, 0x28);
    p16(img + 6, (uint16_t)cnt);
    p16(img + 0x28, 1);
    for (uint32_t i = 1; i < cnt; i++) {
        uint32_t pos = i * 512 - 2;
        img[0x28 + 2 * i] = img[pos];
        img[0x28 + 2 * i + 1] = img[pos + 1];
        p16(img + pos, 1);
    }
}

/* Eintrag in Knoten-Abbild schreiben */
static uint32_t node_put(uint8_t *img, uint32_t off, const IEnt *x, int with_child) {
    memcpy(img + off, x->e, x->len);
    p16(img + off + 8, (uint16_t)(x->len + (with_child ? 8 : 0)));
    p16(img + off + 12, with_child ? 1 : 0);
    if (with_child) p64(img + off + x->len, x->child);
    return x->len + (with_child ? 8 : 0);
}

static uint32_t node_last(uint8_t *img, uint32_t off, int with_child, uint64_t child) {
    p64(img + off, 0);
    p16(img + off + 8, (uint16_t)(with_child ? 24 : 16));
    p16(img + off + 10, 0);
    p16(img + off + 12, (uint16_t)(2 | (with_child ? 1 : 0)));
    if (with_child) p64(img + off + 16, child);
    return with_child ? 24 : 16;
}

/* Der Index wird aus der sortierten Eintragsliste komplett neu aufgebaut (B-Baum von unten nach oben).
 * 'L' wird verbraucht. Kleine Verzeichnisse bleiben im MFT-Record, grosse bekommen $INDEX_ALLOCATION. */
static int dir_rebuild(Ntfs *n, uint64_t dirrec, IList *L) {
    IList cur = *L, next;
    memset(L, 0, sizeof *L);
    memset(&next, 0, sizeof next);
    uint8_t **nodes = 0;
    uint32_t nn = 0, ncap = 0;
    uint32_t *sep = 0;
    uint8_t *rec = kapi.malloc(n->rec_size);
    uint8_t *rootbuf = 0, *rootval = 0, *abuf = 0, *bmv = 0;
    Attr al;
    memset(&al, 0, sizeof al);
    Run nr[32];
    uint32_t nnr = 0;
    int r = 0;
    if (!rec) { il_free(&cur); return FS_ENOSPC; }

    r = read_record(n, dirrec, rec);
    if (r) goto out;
    uint32_t roff = rec_find(rec, AT_INDEX_ROOT, "$I30");
    if (!roff || rec[roff + 8]) { r = FS_ENOTDIR; goto out; }
    uint8_t hdr[16];
    memcpy(hdr, rec + roff + le16(rec + roff + 0x14), 16);
    uint32_t bsize = le32(hdr + 8);
    if (bsize < 512 || bsize > 65536 || (bsize % 512)) { r = FS_EIO; goto out; }
    uint32_t aoff = rec_find(rec, AT_INDEX_ALLOC, "$I30");
    int force_large = aoff != 0;
    if (aoff) {
        r = process_attr(&al, rec + aoff);
        if (r) goto out;
    }
    uint32_t used = le32(rec + 0x18), alloc = le32(rec + 0x1C), root_old = le32(rec + roff + 4);
    uint32_t root_room = alloc - (used - root_old);

    uint32_t usa_cnt = bsize / 512 + 1, estart = ALIGN8(0x28 + 2 * usa_cnt), cap = bsize - estart - 24;
    uint32_t per = bsize >= n->cbytes ? bsize / n->cbytes : bsize / 512;
    int leaf = 1;
    uint64_t last_child = 0;

    for (;;) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < cur.n; i++) sum += cur.v[i].len + (leaf ? 0 : 8);
        uint32_t last_len = leaf ? 16 : 24;
        uint32_t alen = ALIGN8(0x20 + 32 + sum + last_len);
        if (alen <= root_room && !(force_large && leaf)) break;

        /* Aufteilen: Trenner wandern in die naechste Ebene */
        sep = kapi.malloc((cur.n + 1) * sizeof(uint32_t));
        if (!sep) { r = FS_ENOSPC; goto out; }
        uint32_t ns = 0, ub = 0;
        for (uint32_t i = 0; i < cur.n; i++) {
            uint32_t xl = cur.v[i].len + (leaf ? 0 : 8);
            if (ub + xl > cap) {
                if (i == cur.n - 1 && i > 0) { sep[ns++] = i - 1; break; }   /* letzter Knoten darf nicht leer sein */
                sep[ns++] = i;
                ub = 0;
                continue;
            }
            ub += xl;
        }
        uint32_t startk = 0;
        uint64_t new_last = 0;
        for (uint32_t k = 0; k <= ns; k++) {
            uint32_t endk = k < ns ? sep[k] : cur.n;
            uint8_t *img = kapi.malloc(bsize);
            if (!img) { r = FS_ENOSPC; goto out; }
            memset(img, 0, bsize);
            if (nn == ncap) {
                uint32_t nc = ncap ? ncap * 2 : 8;
                uint8_t **nv = kapi.realloc(nodes, nc * sizeof(uint8_t *));
                if (!nv) { kapi.free(img); r = FS_ENOSPC; goto out; }
                nodes = nv;
                ncap = nc;
            }
            uint32_t off = estart;
            for (uint32_t j = startk; j < endk; j++) {
                off += node_put(img, off, &cur.v[j], !leaf);
                kapi.free(cur.v[j].e);
                cur.v[j].e = 0;
            }
            uint64_t lchild = leaf ? 0 : (k < ns ? cur.v[sep[k]].child : last_child);
            off += node_last(img, off, !leaf, lchild);
            uint64_t vcn = (uint64_t)nn * per;
            p64(img + 0x10, vcn);
            p32(img + 0x18, estart - 0x18);
            p32(img + 0x1C, off - 0x18);
            p32(img + 0x20, bsize - 0x18);
            img[0x24] = leaf ? 0 : 1;
            nodes[nn++] = img;
            if (k < ns) {
                IEnt s = cur.v[sep[k]];
                cur.v[sep[k]].e = 0;
                r = il_insert(&next, next.n, s.e, s.len);
                if (r) { kapi.free(s.e); goto out; }
                next.v[next.n - 1].child = vcn;
                startk = sep[k] + 1;
            } else {
                new_last = vcn;
            }
        }
        kapi.free(sep);
        sep = 0;
        kapi.free(cur.v);
        cur = next;
        memset(&next, 0, sizeof next);
        leaf = 0;
        last_child = new_last;
    }

    /* Wurzel */
    {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < cur.n; i++) sum += cur.v[i].len + (leaf ? 0 : 8);
        uint32_t last_len = leaf ? 16 : 24;
        uint32_t vlen = 32 + sum + last_len;
        rootval = kapi.malloc(vlen);
        rootbuf = kapi.malloc(vlen + 0x40);
        if (!rootval || !rootbuf) { r = FS_ENOSPC; goto out; }
        memset(rootval, 0, vlen);
        memcpy(rootval, hdr, 16);
        p32(rootval + 0x10, 16);
        p32(rootval + 0x14, 16 + sum + last_len);
        p32(rootval + 0x18, 16 + sum + last_len);
        rootval[0x1C] = nn > 0 ? 1 : 0;
        uint32_t off = 32;
        for (uint32_t i = 0; i < cur.n; i++) off += node_put(rootval, off, &cur.v[i], !leaf);
        node_last(rootval, off, !leaf, last_child);
        uint32_t rl = mk_res(rootbuf, AT_INDEX_ROOT, "$I30", rootval, vlen, 0);

        uint64_t al_bytes = attr_clusters(&al) * n->cbytes;
        if (nn > 0 || aoff) {
            uint64_t need_bytes = (uint64_t)nn * bsize;
            if (need_bytes > al_bytes) {
                r = attr_grow(n, &al, (need_bytes - al_bytes + n->cbytes - 1) / n->cbytes, nr, &nnr);
                if (r) goto out;
                al_bytes = attr_clusters(&al) * n->cbytes;
            }
        }
        r = rec_put_attr(rec, AT_INDEX_ROOT, "$I30", rootbuf, rl);
        if (r) goto out;
        if (nn > 0 || aoff) {
            uint32_t acap = 0x100 + al.nruns * 18;
            abuf = kapi.malloc(acap);
            if (!abuf) { r = FS_ENOSPC; goto out; }
            int el = mk_nonres(abuf, acap, AT_INDEX_ALLOC, "$I30", al.runs, al.nruns, al_bytes, al_bytes, al_bytes, n->cbytes);
            r = el < 0 ? FS_ENOSPC : rec_put_attr(rec, AT_INDEX_ALLOC, "$I30", abuf, (uint32_t)el);
            if (r) goto out;
            uint64_t blocks = al_bytes / bsize;
            uint32_t bl = ALIGN8((uint32_t)((blocks + 7) / 8));
            bmv = kapi.malloc(bl + 0x40);
            uint8_t *bbuf = kapi.malloc(bl + 0x40);
            if (!bmv || !bbuf) { kapi.free(bbuf); r = FS_ENOSPC; goto out; }
            memset(bmv, 0, bl);
            for (uint32_t i = 0; i < nn; i++) bit_put(bmv, i, 1);
            uint32_t bll = mk_res(bbuf, AT_BITMAP, "$I30", bmv, bl, 0);
            r = rec_put_attr(rec, AT_BITMAP, "$I30", bbuf, bll);
            kapi.free(bbuf);
            if (r) goto out;
            for (uint32_t i = 0; !r && i < nn; i++) {
                indx_protect(nodes[i], bsize);
                r = attr_write(n, &al, (uint64_t)i * bsize, nodes[i], bsize);
            }
            if (r) goto out;
        }
        r = write_record(n, dirrec, rec);
    }

out:
    if (r) rollback_new(n, nr, nnr);
    for (uint32_t i = 0; i < nn; i++) kapi.free(nodes[i]);
    kapi.free(nodes);
    kapi.free(sep);
    kapi.free(rootval);
    kapi.free(rootbuf);
    kapi.free(abuf);
    kapi.free(bmv);
    attr_free(&al);
    kapi.free(rec);
    il_free(&cur);
    il_free(&next);
    return r;
}

enum { OP_INSERT = 1, OP_DELETE = 2, OP_REPLACE = 3 };

/* Eintrag 'entry' (Schluessel = $FILE_NAME-Inhalt) im Verzeichnis einfuegen, loeschen oder ersetzen */
static int dir_edit(Ntfs *n, uint64_t dirrec, int op, const uint8_t *entry) {
    Dir d;
    int r = dir_open(n, dirrec, &d);
    if (r) return r;
    IList L;
    memset(&L, 0, sizeof L);
    r = walk_dir(n, &d, collect_cb, &L);
    dir_close(&d);
    if (r) { il_free(&L); return r; }

    const uint8_t *kn = entry + 0x10 + 0x42;
    int kl = entry[0x10 + 0x40];
    uint32_t elen = 16 + ALIGN8(le16(entry + 10));
    int idx = -1;
    uint32_t ins = L.n;
    for (uint32_t i = 0; i < L.n; i++) {
        const uint8_t *ek = L.v[i].e + 0x10;
        if (ucmp(n, kn, kl, ek + 0x42, ek[0x40]) == 0) { idx = (int)i; break; }
    }
    if (op == OP_INSERT) {
        if (idx >= 0) r = FS_EEXIST;
        else {
            for (uint32_t i = 0; i < L.n; i++) {
                const uint8_t *ek = L.v[i].e + 0x10;
                if (ucmp(n, kn, kl, ek + 0x42, ek[0x40]) < 0) { ins = i; break; }
            }
            uint8_t *c = kapi.malloc(elen);
            if (!c) r = FS_ENOSPC;
            else {
                memcpy(c, entry, elen);
                p16(c + 8, (uint16_t)elen);
                p16(c + 12, 0);
                r = il_insert(&L, ins, c, elen);
                if (r) kapi.free(c);
            }
        }
    } else if (idx < 0) {
        r = FS_ENOENT;
    } else if (op == OP_DELETE) {
        kapi.free(L.v[idx].e);
        memmove(&L.v[idx], &L.v[idx + 1], (L.n - (uint32_t)idx - 1) * sizeof(IEnt));
        L.n--;
    } else {   /* OP_REPLACE */
        uint8_t *c = kapi.malloc(elen);
        if (!c) r = FS_ENOSPC;
        else {
            memcpy(c, entry, elen);
            p16(c + 8, (uint16_t)elen);
            p16(c + 12, 0);
            kapi.free(L.v[idx].e);
            L.v[idx].e = c;
            L.v[idx].len = elen;
        }
    }
    if (!r) r = dir_rebuild(n, dirrec, &L);
    il_free(&L);
    return r;
}

/* ------------------------------------------------------------------ */
/* Namen und neue Records                                              */
/* ------------------------------------------------------------------ */
static int nt_valid_name(const char *s) {
    size_t len = slen(s);
    if (len == 0 || len > 255) return 0;
    if (s[len - 1] == ' ' || s[len - 1] == '.') return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) return 0;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    return 1;
}

static int ascii_key(const char *name, uint8_t *key, int *klen) {
    int k = 0;
    for (const char *c = name; *c; c++) {
        if ((uint8_t)*c > 127 || k >= 255) return FS_ENAME;
        key[2 * k] = (uint8_t)*c;
        key[2 * k + 1] = 0;
        k++;
    }
    *klen = k;
    return 0;
}

/* 3 = Win32 + DOS (passt als 8.3 in Grossbuchstaben), sonst 1 = Win32 */
static uint8_t ns_for(const char *name) {
    const char *dot = 0;
    size_t len = slen(name);
    for (const char *p = name; *p; p++)
        if (*p == '.') { if (dot) return 1; dot = p; }
    size_t bl = dot ? (size_t)(dot - name) : len, el = dot ? len - bl - 1 : 0;
    if (bl == 0 || bl > 8 || el > 3) return 1;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') continue;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '$' || c == '~' || c == '!' || c == '#' || c == '%' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '@' || c == '^' || c == '`' || c == '{' || c == '}')) return 1;
    }
    return 3;
}

static uint32_t build_fn(uint8_t *out, uint64_t parent_ref, const char *name, uint8_t ns, uint64_t alloc, uint64_t real,
                         uint32_t fflags, uint64_t t) {
    uint32_t nl = (uint32_t)slen(name);
    memset(out, 0, 0x42 + 2 * nl);
    p64(out, parent_ref);
    p64(out + 8, t); p64(out + 0x10, t); p64(out + 0x18, t); p64(out + 0x20, t);
    p64(out + 0x28, alloc);
    p64(out + 0x30, real);
    p32(out + 0x38, fflags);
    out[0x40] = (uint8_t)nl;
    out[0x41] = ns;
    for (uint32_t i = 0; i < nl; i++) p16(out + 0x42 + 2 * i, (uint16_t)(uint8_t)name[i]);
    return 0x42 + 2 * nl;
}

static uint32_t build_entry(uint8_t *out, uint64_t ref, const uint8_t *fn, uint32_t fnlen) {
    uint32_t len = 16 + ALIGN8(fnlen);
    memset(out, 0, len);
    p64(out, ref);
    p16(out + 8, (uint16_t)len);
    p16(out + 10, (uint16_t)fnlen);
    memcpy(out + 16, fn, fnlen);
    return len;
}

/* wie rec_put_attr, aber immer einfuegen (z.B. weiteres $FILE_NAME bei Hardlinks/Umbenennen) */
static int rec_add_attr(uint8_t *rec, uint32_t type, const uint8_t *attr, uint32_t len) {
    uint32_t used = le32(rec + 0x18), alloc = le32(rec + 0x1C);
    if (used + len > alloc) return FS_ENOSPC;
    uint32_t pos = le16(rec + 0x14);
    while (pos + 16 <= used) {
        uint32_t t = le32(rec + pos);
        if (t == AT_END || t > type) break;
        pos += le32(rec + pos + 4);
    }
    uint16_t inst = le16(rec + 0x28);
    p16(rec + 0x28, (uint16_t)(inst + 1));
    memmove(rec + pos + len, rec + pos, used - pos);
    memcpy(rec + pos, attr, len);
    p16(rec + pos + 0xE, inst);
    p32(rec + 0x18, used + len);
    return 0;
}

/* Pfad "/a/b/name[:strom]" -> Elternpfad, Name, Strom */
static int split_parent(const char *path, char *parent, char *leaf, char *stream) {
    size_t len = slen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t i = len;
    while (i > 0 && path[i - 1] != '/') i--;
    if (i >= len) return FS_EINVAL;
    size_t cl = len - i, ll = cl;
    stream[0] = 0;
    for (size_t k = 0; k < cl; k++)
        if (path[i + k] == ':') {
            ll = k;
            if (cl - k - 1 > 63) return FS_ENAME;
            for (size_t m = 0; m < cl - k - 1; m++) stream[m] = path[i + k + 1 + m];
            stream[cl - k - 1] = 0;
            break;
        }
    if (ll == 0 || ll > 255) return FS_ENAME;
    for (size_t k = 0; k < ll; k++) leaf[k] = path[i + k];
    leaf[ll] = 0;
    if (i <= 1) { parent[0] = '/'; parent[1] = 0; }
    else {
        for (size_t k = 0; k < i - 1; k++) parent[k] = path[k];
        parent[i - 1] = 0;
    }
    return 0;
}

/* Elternverzeichnis aufloesen und pruefen, dass es ein Verzeichnis ist */
static int parent_dir(Ntfs *n, const char *parent, uint64_t *dirrec) {
    char st[64];
    int r = resolve(n, parent, dirrec, st);
    if (r) return r;
    int isd;
    r = rec_is_dir(n, *dirrec, &isd);
    if (r) return r;
    return isd ? 0 : FS_ENOTDIR;
}

/* neuen MFT-Record im Speicher aufbauen (Record-Nummer wird belegt) */
static int create_record(Ntfs *n, uint64_t dirrec, const char *leaf, int is_dir, uint64_t *recno_out, uint8_t *rec) {
    uint8_t *prec = kapi.malloc(n->rec_size);
    uint8_t *old = kapi.malloc(n->rec_size);
    uint8_t *ab = kapi.malloc(1024);
    int r = 0;
    uint64_t recno = 0;
    int have_rec = 0;
    if (!prec || !old || !ab) { r = FS_ENOSPC; goto out; }
    r = read_record(n, dirrec, prec);
    if (r) goto out;
    uint16_t pseq = le16(prec + 0x10);
    uint32_t sec = 0x100;
    uint32_t si = rec_find(prec, AT_STD_INFO, 0);
    if (si && !prec[si + 8] && le32(prec + si + 0x10) >= 0x48) sec = le32(prec + si + le16(prec + si + 0x14) + 0x34);

    r = mrec_alloc(n, &recno);
    if (r) goto out;
    have_rec = 1;
    uint16_t seq = 1;
    if (read_record_raw(n, recno, old) == 0) {
        seq = le16(old + 0x10);
        if (!seq) seq = 1;
    }

    uint32_t uc = n->rec_size / 512 + 1, uo = 0x30, ao = ALIGN8(uo + 2 * uc);
    memset(rec, 0, n->rec_size);
    memcpy(rec, "FILE", 4);
    p16(rec + 4, (uint16_t)uo);
    p16(rec + 6, (uint16_t)uc);
    p16(rec + 0x10, seq);
    p16(rec + 0x12, 1);
    p16(rec + 0x14, (uint16_t)ao);
    p16(rec + 0x16, (uint16_t)(1 | (is_dir ? 2 : 0)));
    p32(rec + 0x1C, n->rec_size);
    p32(rec + 0x2C, (uint32_t)recno);
    p16(rec + uo, 1);
    p32(rec + ao, AT_END);
    p32(rec + 0x18, ao + 8);

    uint64_t t = now_ntfs();
    uint32_t fflags = is_dir ? 0x10000000u : 0x20u;
    uint8_t siv[0x48];
    memset(siv, 0, sizeof siv);
    p64(siv, t); p64(siv + 8, t); p64(siv + 0x10, t); p64(siv + 0x18, t);
    p32(siv + 0x20, fflags);
    p32(siv + 0x34, sec);
    uint32_t l = mk_res(ab, AT_STD_INFO, 0, siv, sizeof siv, 0);
    r = rec_add_attr(rec, AT_STD_INFO, ab, l);
    if (r) goto out;

    uint8_t fn[0x42 + 512];
    uint32_t fl = build_fn(fn, dirrec | ((uint64_t)pseq << 48), leaf, ns_for(leaf), 0, 0, fflags, t);
    l = mk_res(ab, AT_FILE_NAME, 0, fn, fl, 1);
    r = rec_add_attr(rec, AT_FILE_NAME, ab, l);
    if (r) goto out;

    if (is_dir) {
        uint8_t rv[16 + 16 + 16];
        memset(rv, 0, sizeof rv);
        p32(rv, AT_FILE_NAME);
        p32(rv + 4, 1);
        p32(rv + 8, n->idx_size);
        rv[0xC] = (uint8_t)(n->idx_size >= n->cbytes ? n->idx_size / n->cbytes : n->idx_size / 512);
        p32(rv + 0x10, 16);
        p32(rv + 0x14, 32);
        p32(rv + 0x18, 32);
        p16(rv + 0x20 + 8, 16);
        p16(rv + 0x20 + 12, 2);
        l = mk_res(ab, AT_INDEX_ROOT, "$I30", rv, sizeof rv, 0);
        r = rec_add_attr(rec, AT_INDEX_ROOT, ab, l);
    } else {
        l = mk_res(ab, AT_DATA, 0, 0, 0, 0);
        r = rec_add_attr(rec, AT_DATA, ab, l);
    }
    if (!r) *recno_out = recno;
out:
    if (r && have_rec) mrec_free(n, recno);
    kapi.free(prec);
    kapi.free(old);
    kapi.free(ab);
    return r;
}

/* Zeiten und Groessen in $STANDARD_INFORMATION / $FILE_NAME nachfuehren, Record schreiben und die
 * Verzeichniseintraege (Index) abgleichen. mode: OP_INSERT (neu) oder OP_REPLACE (geaendert). */
static int names_sync(Ntfs *n, uint64_t recno, uint8_t *rec, int mode, int sizes) {
    uint64_t t = now_ntfs();
    uint32_t si = rec_find(rec, AT_STD_INFO, 0);
    if (si && !rec[si + 8]) {
        uint8_t *v = rec + si + le16(rec + si + 0x14);
        p64(v + 8, t); p64(v + 0x10, t); p64(v + 0x18, t);
    }
    int isdir = le16(rec + 0x16) & 2;
    uint64_t alloc = 0, real = 0;
    uint32_t d = rec_find(rec, AT_DATA, 0);
    if (d && !isdir) {
        if (!rec[d + 8]) { real = le32(rec + d + 0x10); alloc = ALIGN8((uint32_t)real); }
        else { alloc = le64(rec + d + 0x28); real = le64(rec + d + 0x30); }
    }
    for (uint32_t off = rec_first(rec); off; off = rec_next(rec, off)) {
        if (le32(rec + off) != AT_FILE_NAME || rec[off + 8]) continue;
        uint8_t *v = rec + off + le16(rec + off + 0x14);
        p64(v + 0x10, t); p64(v + 0x18, t); p64(v + 0x20, t);
        if (sizes && !isdir) { p64(v + 0x28, alloc); p64(v + 0x30, real); }
    }
    int r = write_record(n, recno, rec);
    if (r) return r;
    uint64_t ref = recno | ((uint64_t)le16(rec + 0x10) << 48);
    uint8_t *entry = kapi.malloc(16 + ALIGN8(0x42 + 512));
    if (!entry) return FS_ENOSPC;
    for (uint32_t off = rec_first(rec); !r && off; off = rec_next(rec, off)) {
        if (le32(rec + off) != AT_FILE_NAME || rec[off + 8]) continue;
        const uint8_t *v = rec + off + le16(rec + off + 0x14);
        uint32_t fl = le32(rec + off + 0x10);
        build_entry(entry, ref, v, fl);
        r = dir_edit(n, le64(v) & REF_MASK, mode, entry);
    }
    kapi.free(entry);
    return r;
}

static int dir_empty_cb(Ntfs *n, const uint8_t *e, void *ctx) {
    (void)n; (void)e; (void)ctx;
    return 1;
}

static int dir_is_empty(Ntfs *n, uint64_t recno, int *empty) {
    Dir d;
    int r = dir_open(n, recno, &d);
    if (r) return r;
    r = walk_dir(n, &d, dir_empty_cb, 0);
    dir_close(&d);
    if (r < 0) return r;
    *empty = (r == 0);
    return 0;
}

static int rec_dirflag(const uint8_t *rec) { return (le16(rec + 0x16) & 2) != 0; }

/* ------------------------------------------------------------------ */
/* Fs-Operationen (Schreiben)                                          */
/* ------------------------------------------------------------------ */
static int n_write(Fs *fs, const char *path, const void *buf, uint64_t len, int append) {
    Ntfs *n = fs->priv;
    char parent[512], leaf[256], stream[64];
    int r = split_parent(path, parent, leaf, stream);
    if (r) return r;
    uint64_t dirrec, recno = 0;
    r = parent_dir(n, parent, &dirrec);
    if (r) return r;
    uint8_t key[512];
    int kl;
    r = ascii_key(leaf, key, &kl);
    if (r) return r;
    int ex = dir_lookup(n, dirrec, key, kl, &recno);
    if (ex && ex != FS_ENOENT) return ex;
    if (ex == FS_ENOENT && !nt_valid_name(leaf)) return FS_ENAME;
    if (ex == FS_ENOENT && stream[0]) return FS_ENOENT;

    r = begin_write(n);
    if (r) return r;
    uint8_t *rec = kapi.malloc(n->rec_size);
    if (!rec) { end_write(n); return FS_ENOSPC; }
    const char *st = stream[0] ? stream : 0;
    if (ex == 0) {
        r = read_record(n, recno, rec);
        if (!r && rec_dirflag(rec) && !st) r = FS_EISDIR;
        if (!r && recno < 24) r = FS_EINVAL;
        if (!r) r = stream_set(n, rec, st, buf, len, append);
        if (!r) r = names_sync(n, recno, rec, OP_REPLACE, st == 0);
    } else {
        r = create_record(n, dirrec, leaf, 0, &recno, rec);
        if (!r) {
            r = stream_set(n, rec, 0, buf, len, 0);
            if (!r) r = names_sync(n, recno, rec, OP_INSERT, 1);
            if (r) mrec_free(n, recno);
        }
    }
    kapi.free(rec);
    int r2 = end_write(n);
    return r ? r : r2;
}

static int n_mkdir(Fs *fs, const char *path) {
    Ntfs *n = fs->priv;
    char parent[512], leaf[256], stream[64];
    int r = split_parent(path, parent, leaf, stream);
    if (r) return r;
    if (stream[0] || !nt_valid_name(leaf)) return FS_ENAME;
    uint64_t dirrec, recno;
    r = parent_dir(n, parent, &dirrec);
    if (r) return r;
    uint8_t key[512];
    int kl;
    r = ascii_key(leaf, key, &kl);
    if (r) return r;
    int ex = dir_lookup(n, dirrec, key, kl, &recno);
    if (ex == 0) return FS_EEXIST;
    if (ex != FS_ENOENT) return ex;

    r = begin_write(n);
    if (r) return r;
    uint8_t *rec = kapi.malloc(n->rec_size);
    if (!rec) { end_write(n); return FS_ENOSPC; }
    r = create_record(n, dirrec, leaf, 1, &recno, rec);
    if (!r) {
        r = names_sync(n, recno, rec, OP_INSERT, 0);
        if (r) mrec_free(n, recno);
    }
    kapi.free(rec);
    int r2 = end_write(n);
    return r ? r : r2;
}

#define MAX_NAMES 8

static int n_remove(Fs *fs, const char *path) {
    Ntfs *n = fs->priv;
    char parent[512], leaf[256], stream[64];
    int r = split_parent(path, parent, leaf, stream);
    if (r) return r;
    if (stream[0]) return FS_ENOSYS;
    uint64_t dirrec, recno;
    r = parent_dir(n, parent, &dirrec);
    if (r) return r;
    uint8_t key[512];
    int kl;
    r = ascii_key(leaf, key, &kl);
    if (r) return r;
    r = dir_lookup(n, dirrec, key, kl, &recno);
    if (r) return r;
    if (recno < 24) return FS_EINVAL;

    r = begin_write(n);
    if (r) return r;
    uint8_t *rec = kapi.malloc(n->rec_size);
    uint8_t *names = kapi.malloc((size_t)MAX_NAMES * 640);
    uint8_t *entry = kapi.malloc(16 + ALIGN8(0x42 + 512));
    if (!rec || !names || !entry) r = FS_ENOSPC;
    uint32_t cnt_names = 0;
    uint16_t lens[MAX_NAMES];
    if (!r) r = read_record(n, recno, rec);
    if (!r && rec_find(rec, AT_ATTR_LIST, 0)) r = FS_ENOSYS;
    if (!r && rec_dirflag(rec)) {
        int empty = 0;
        r = dir_is_empty(n, recno, &empty);
        if (!r && !empty) r = FS_ENOTEMPTY;
    }
    if (!r) {   /* Namen dieses Elternverzeichnisses sammeln und aus dem Record nehmen */
        uint32_t offs[MAX_NAMES], no = 0;
        uint32_t same_dir = 0;   /* Namen (ohne DOS-Alias) dieses Records in diesem Verzeichnis */
        for (uint32_t off = rec_first(rec); off; off = rec_next(rec, off)) {
            if (le32(rec + off) != AT_FILE_NAME || rec[off + 8]) continue;
            const uint8_t *v = rec + off + le16(rec + off + 0x14);
            if ((le64(v) & REF_MASK) == dirrec && v[0x41] != 2) same_dir++;
        }
        for (uint32_t off = rec_first(rec); off; off = rec_next(rec, off)) {
            if (le32(rec + off) != AT_FILE_NAME || rec[off + 8]) continue;
            const uint8_t *v = rec + off + le16(rec + off + 0x14);
            if ((le64(v) & REF_MASK) != dirrec) continue;
            /* nur den gemeinten Namen; der DOS-Alias geht mit, wenn es keinen weiteren Namen hier gibt */
            int match = ucmp(n, key, kl, v + 0x42, v[0x40]) == 0 || (v[0x41] == 2 && same_dir == 1);
            if (!match) continue;
            if (no >= MAX_NAMES) { r = FS_ENOSYS; break; }
            uint32_t fl = le32(rec + off + 0x10);
            memcpy(names + (size_t)no * 640, v, fl);
            lens[no] = (uint16_t)fl;
            offs[no++] = off;
        }
        cnt_names = no;
        for (uint32_t i = no; !r && i > 0; i--) rec_remove_attr(rec, offs[i - 1]);
    }
    uint64_t ref = recno | ((uint64_t)le16(rec ? rec + 0x10 : (uint8_t *)"\0\0") << 48);
    for (uint32_t i = 0; !r && i < cnt_names; i++) {
        build_entry(entry, ref, names + (size_t)i * 640, lens[i]);
        r = dir_edit(n, dirrec, OP_DELETE, entry);
    }
    if (!r) {
        uint32_t remaining = 0;
        for (uint32_t off = rec_first(rec); off; off = rec_next(rec, off))
            if (le32(rec + off) == AT_FILE_NAME && !rec[off + 8] && rec[off + le16(rec + off + 0x14) + 0x41] != 2) remaining++;
        if (remaining > 0) {   /* weitere Hardlinks: Record bleibt */
            p16(rec + 0x12, (uint16_t)remaining);
            r = write_record(n, recno, rec);
        } else {
            for (uint32_t off = rec_first(rec); !r && off; off = rec_next(rec, off))
                r = attr_free_clusters(n, rec + off);
            uint16_t seq = (uint16_t)(le16(rec + 0x10) + 1);
            if (seq == 0) seq = 1;
            p16(rec + 0x10, seq);
            p16(rec + 0x16, 0);   /* nicht mehr in Benutzung */
            if (!r) r = write_record(n, recno, rec);
            if (!r) r = mrec_free(n, recno);
        }
    }
    kapi.free(rec);
    kapi.free(names);
    kapi.free(entry);
    int r2 = end_write(n);
    return r ? r : r2;
}

static int n_rename(Fs *fs, const char *from, const char *to) {
    Ntfs *n = fs->priv;
    char p1[512], l1[256], s1[64], p2[512], l2[256], s2[64];
    int r = split_parent(from, p1, l1, s1);
    if (r) return r;
    r = split_parent(to, p2, l2, s2);
    if (r) return r;
    if (s1[0] || s2[0]) return FS_ENOSYS;
    if (!nt_valid_name(l2)) return FS_ENAME;
    uint64_t dir1, dir2, recno, other;
    r = parent_dir(n, p1, &dir1);
    if (r) return r;
    r = parent_dir(n, p2, &dir2);
    if (r) return r;
    uint8_t k1[512], k2[512];
    int kl1, kl2;
    if ((r = ascii_key(l1, k1, &kl1)) || (r = ascii_key(l2, k2, &kl2))) return r;
    r = dir_lookup(n, dir1, k1, kl1, &recno);
    if (r) return r;
    if (recno < 24) return FS_EINVAL;
    int ex = dir_lookup(n, dir2, k2, kl2, &other);
    if (ex == 0 && other != recno) return FS_EEXIST;
    if (ex && ex != FS_ENOENT) return ex;
    {   /* Verzeichnis nicht in sich selbst verschieben */
        size_t fl = slen(from);
        if (slen(to) > fl) {
            int pre = 1;
            for (size_t i = 0; i < fl; i++) {
                char a = from[i], b = to[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { pre = 0; break; }
            }
            if (pre && to[fl] == '/') return FS_EINVAL;
        }
    }

    r = begin_write(n);
    if (r) return r;
    uint8_t *rec = kapi.malloc(n->rec_size);
    uint8_t *names = kapi.malloc((size_t)MAX_NAMES * 640);
    uint8_t *entry = kapi.malloc(16 + ALIGN8(0x42 + 512));
    uint8_t *ab = kapi.malloc(1024);
    uint8_t *p2rec = kapi.malloc(n->rec_size);
    uint16_t lens[MAX_NAMES];
    uint32_t cnt = 0;
    if (!rec || !names || !entry || !ab || !p2rec) r = FS_ENOSPC;
    uint8_t tmpl[0x42 + 512];
    int have_tmpl = 0;
    if (!r) r = read_record(n, recno, rec);
    if (!r && rec_find(rec, AT_ATTR_LIST, 0)) r = FS_ENOSYS;
    if (!r) r = read_record(n, dir2, p2rec);
    if (!r) {
        uint32_t offs[MAX_NAMES], no = 0;
        uint32_t same_dir = 0;
        for (uint32_t off = rec_first(rec); off; off = rec_next(rec, off)) {
            if (le32(rec + off) != AT_FILE_NAME || rec[off + 8]) continue;
            const uint8_t *v = rec + off + le16(rec + off + 0x14);
            if ((le64(v) & REF_MASK) == dir1 && v[0x41] != 2) same_dir++;
        }
        for (uint32_t off = rec_first(rec); off; off = rec_next(rec, off)) {
            if (le32(rec + off) != AT_FILE_NAME || rec[off + 8]) continue;
            const uint8_t *v = rec + off + le16(rec + off + 0x14);
            if ((le64(v) & REF_MASK) != dir1) continue;
            uint32_t fl = le32(rec + off + 0x10);
            int match = ucmp(n, k1, kl1, v + 0x42, v[0x40]) == 0 || (v[0x41] == 2 && same_dir == 1);
            if (!match) continue;
            if (no >= MAX_NAMES) { r = FS_ENOSYS; break; }
            if (!have_tmpl && v[0x41] != 2) { memcpy(tmpl, v, fl); have_tmpl = 1; }
            memcpy(names + (size_t)no * 640, v, fl);
            lens[no] = (uint16_t)fl;
            offs[no++] = off;
        }
        cnt = no;
        if (!r && !have_tmpl) r = FS_EIO;
        for (uint32_t i = no; !r && i > 0; i--) rec_remove_attr(rec, offs[i - 1]);
    }
    uint64_t ref = recno | ((uint64_t)le16(rec + 0x10) << 48);
    for (uint32_t i = 0; !r && i < cnt; i++) {
        build_entry(entry, ref, names + (size_t)i * 640, lens[i]);
        r = dir_edit(n, dir1, OP_DELETE, entry);
    }
    if (!r) {
        uint8_t fn[0x42 + 512];
        uint32_t fl = build_fn(fn, dir2 | ((uint64_t)le16(p2rec + 0x10) << 48), l2, ns_for(l2), le64(tmpl + 0x28),
                               le64(tmpl + 0x30), le32(tmpl + 0x38), le64(tmpl + 8));
        p64(fn + 8, le64(tmpl + 8));   /* Erstellungszeit behalten */
        uint32_t l = mk_res(ab, AT_FILE_NAME, 0, fn, fl, 1);
        r = rec_add_attr(rec, AT_FILE_NAME, ab, l);
        if (!r) r = write_record(n, recno, rec);
        if (!r) {   /* nur den neuen Namen im Zielverzeichnis eintragen */
            build_entry(entry, ref, fn, fl);
            r = dir_edit(n, dir2, OP_INSERT, entry);
        }
    }
    kapi.free(rec); kapi.free(names); kapi.free(entry); kapi.free(ab); kapi.free(p2rec);
    int r2 = end_write(n);
    return r ? r : r2;
}

/* ------------------------------------------------------------------ */
/* Einbinden                                                           */
/* ------------------------------------------------------------------ */
int ntfs_mount(BlockDev *bd, Fs **out) {
    uint8_t *bs = kapi.malloc(bd->sector_size);
    if (!bs) return FS_EIO;
    if (bd->read(bd, 0, 1, bs)) { kapi.free(bs); return FS_EIO; }
    if (!memeq(bs + 3, "NTFS    ", 8) || bs[510] != 0x55 || bs[511] != 0xAA) {
        con_puts("NTFS: BOOTSEKTOR UNGUELTIG (KENNUNG ODER 55AA FEHLT) - KEIN ECHTES NTFS-VOLUME?\n");
        kapi.free(bs);
        return FS_EINVAL;
    }

    Ntfs *n = kapi.malloc(sizeof(Ntfs));
    Fs *fs = kapi.malloc(sizeof(Fs));
    if (!n || !fs) { kapi.free(bs); kapi.free(n); kapi.free(fs); return FS_EIO; }
    memset(n, 0, sizeof *n);
    memset(fs, 0, sizeof *fs);
    n->dev = bd;
    n->bps = le16(bs + 11);
    uint32_t spc = bs[13];
    n->spc = spc > 0x80 ? 1u << (256 - spc) : spc;
    n->total_sectors = le64(bs + 0x28);
    n->mft_lcn = le64(bs + 0x30);
    int8_t cpr = (int8_t)bs[0x40];
    n->cbytes = n->bps * n->spc;
    n->rec_size = cpr > 0 ? (uint32_t)cpr * n->cbytes : 1u << (-cpr);
    n->clusters = n->bps && n->spc ? n->total_sectors / n->spc : 0;
    {
        int8_t cpi = (int8_t)bs[0x44];   /* Clustergroesse eines Indexpuffers */
        n->idx_size = cpi > 0 ? (uint32_t)cpi * n->cbytes : 1u << (-cpi);
        n->mftmirr_lcn = le64(bs + 0x38);
    }
    kapi.free(bs);
    if (n->bps < 512 || n->spc == 0 || n->cbytes > (1u << 21) || n->rec_size < 512 || n->rec_size > 16384 || !n->clusters) {
        con_puts("NTFS: UNGUELTIGE PARAMETER (BYTES/SEKTOR ");
        con_dec(n->bps);
        con_puts(", SEKTOREN/CLUSTER ");
        con_dec(n->spc);
        con_puts(", MFT-EINTRAG ");
        con_dec(n->rec_size);
        con_puts(")\n");
        kapi.free(n); kapi.free(fs);
        return FS_EINVAL;
    }

    if (attr_load(n, 0, AT_DATA, 0, &n->mft) || n->mft.resident) {   /* $MFT:$DATA */
        kapi.free(n); kapi.free(fs);
        return FS_EIO;
    }

    Attr up;   /* $UpCase (Record 10) */
    if (attr_load(n, 10, AT_DATA, 0, &up) == 0 && !up.resident && up.real_size >= 256 && up.real_size <= 131072) {
        uint8_t *tmp = kapi.malloc((size_t)up.real_size);
        n->upcase = kapi.malloc((size_t)up.real_size);
        if (tmp && n->upcase && attr_read(n, &up, 0, tmp, up.real_size) == (int64_t)up.real_size) {
            n->upcase_n = (uint32_t)(up.real_size / 2);
            for (uint32_t i = 0; i < n->upcase_n; i++) n->upcase[i] = le16(tmp + 2 * i);
        } else {
            kapi.free(n->upcase);
            n->upcase = 0;
        }
        kapi.free(tmp);
    }
    attr_free(&up);

    char label[16] = "";
    Attr vn;   /* $Volume (Record 3): Volumenname */
    if (attr_load(n, 3, AT_VOL_NAME, 0, &vn) == 0) {
        utf16_to_ascii(vn.rdata, (int)(vn.rlen / 2 > 15 ? 15 : vn.rlen / 2), label, sizeof label);
        attr_free(&vn);
    }
    scopy(fs->label, label[0] ? label : "NO NAME", sizeof fs->label);
    scopy(fs->type, "NTFS", sizeof fs->type);
    fs->dev = bd;
    fs->priv = n;
    fs->readonly = 1;
    fs->stat = n_stat;
    fs->readdir = n_readdir;
    fs->read = n_read;
    fs->statfs = n_statfs;
    fs->write = n_write;
    fs->mkdir = n_mkdir;
    fs->remove = n_remove;
    fs->rename = n_rename;
    fs->enable_write = n_enable_write;   /* Schreiben nur bei sauberem Volume */
    *out = fs;
    return 0;
}
