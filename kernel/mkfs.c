/* SPDX-License-Identifier: MIT */
/*
 * mkfs.c - NTFS (Version 3.1), FAT32 und GPT schreiben
 *
 * Kein Kernel-Include: nur mkfs.h (MkDev). Siehe dort.
 *
 * NTFS: Der Aufbau folgt dem, was mkntfs (ntfs-3g) fuer ein frisches Volume schreibt (Referenz zum Vergleichen mit
 * tools/ntfsdump.py): Systemdateien $MFT bis $Extend (MFT-Eintraege 0-11), reservierte Eintraege 12-15 (belegt) und 16-23
 * (frei). Feste Daten (Bootsektor-Vorlage, $AttrDef, Sicherheitsdeskriptoren, $UpCase-Laufliste) stehen in ntfs_tmpl.inc,
 * erzeugt aus einem mkntfs-Volume (tools/gen-ntfs-tmpl.py). $LogFile ist komplett 0xFF: das ist der Zustand "leer, kein
 * Wiederherstellen noetig", den auch chkdsk und ntfs-3g herstellen; Windows initialisiert das Protokoll beim ersten Mounten.
 * Nicht angelegt werden $Quota, $ObjId und $Reparse (Eintraege 24-26): $Extend bleibt leer, Windows legt sie bei Bedarf an.
 *
 * Aufbau auf dem Datentraeger (Cluster zu 4096 Bytes):
 *   LCN 0-1     $Boot (Bootsektor + 8 KiB), Sicherungs-Bootsektor im letzten Sektor des Volumes
 *   LCN 2       $MFT-Bitmap
 *   LCN 4 ...   $MFT (24 Eintraege), danach die MFT-Zone (12,5 % des Volumes) bis zu den restlichen Metadaten
 *   dahinter    Wurzel-Sicherheitsdeskriptor, Wurzel-Indexblock, $AttrDef, $Bitmap, $Secure/$SDS, $UpCase
 *   Mitte       $MFTMirr (Kopie der Eintraege 0-3), danach $LogFile
 */
#include "mkfs.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;

#include "ntfs_tmpl.inc"

/* ------------------------------------------------------------------ */
/* Kleine Helfer                                                       */
/* ------------------------------------------------------------------ */
static void mset(void *p, int v, size_t n) {
    u8 *b = p;
    for (size_t i = 0; i < n; i++) b[i] = (u8)v;
}

static void mcpy(void *d, const void *s, size_t n) {
    u8 *a = d;
    const u8 *b = s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void p16(u8 *b, u32 o, u32 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
static void p32(u8 *b, u32 o, u32 v) { p16(b, o, v); p16(b, o + 2, v >> 16); }
static void p64(u8 *b, u32 o, u64 v) { p32(b, o, (u32)v); p32(b, o + 4, (u32)(v >> 32)); }

static u64 al8(u64 v) { return (v + 7) & ~7ULL; }
static u64 cdiv(u64 a, u64 b) { return (a + b - 1) / b; }

static void progress(MkDev *d, const char *what, int pct) {
    if (d->progress) d->progress(d->ctx, what, pct);
}

/* Bytes ab einem sektorausgerichteten Byte-Offset schreiben; die Laenge wird auf volle Sektoren mit Nullen aufgefuellt */
static int wr(MkDev *d, u64 off, const void *buf, u64 len) {
    u32 ss = d->sector_size;
    if (off % ss) return MKFS_EPARAM;
    u64 full = len / ss * ss;
    if (full) {
        /* in Stuecken von hoechstens 64 KiB, damit Treiber mit kleinem Zwischenpuffer nicht stolpern */
        u64 done = 0;
        while (done < full) {
            u64 n = full - done;
            if (n > 65536) n = 65536;
            if (d->write(d->ctx, (off + done) / ss, (u32)(n / ss), (const u8 *)buf + done) != 0) return MKFS_EIO;
            done += n;
        }
    }
    if (len > full) {
        u8 *t = d->alloc(ss);
        if (!t) return MKFS_ENOMEM;
        mset(t, 0, ss);
        mcpy(t, (const u8 *)buf + full, (size_t)(len - full));
        int r = d->write(d->ctx, (off + full) / ss, 1, t);
        d->release(t);
        if (r) return MKFS_EIO;
    }
    return 0;
}

static int fill(MkDev *d, u64 off, u64 len, int val) {
    u8 *t = d->alloc(65536);
    if (!t) return MKFS_ENOMEM;
    mset(t, val, 65536);
    int r = 0;
    for (u64 done = 0; done < len && !r;) {
        u64 n = len - done;
        if (n > 65536) n = 65536;
        r = wr(d, off + done, t, n);
        done += n;
    }
    d->release(t);
    return r;
}

/* ------------------------------------------------------------------ */
/* NTFS                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    MkDev *d;
    u32 bps, spc, cs, rec;   /* Sektor, Sektoren je Cluster, Cluster, MFT-Eintragsgroesse (Bytes) */
    u64 nclusters;
    u64 t;                   /* Zeit */
} Vol;

typedef struct {
    u8 *b;
    u32 used;
    u16 next_id;
    u32 rec;
} Rec;

typedef struct {
    u64 lcn;     /* ~0 = Loch (sparse) */
    u64 len;
} Run;

#define NOLCN (~0ULL)

static void rec_begin(Vol *v, Rec *r, u8 *buf, u32 recno, u16 seq, u16 flags, u16 links) {
    r->b = buf;
    r->rec = v->rec;
    mset(buf, 0, v->rec);
    u32 usa_n = v->rec / 512 + 1;
    buf[0] = 'F'; buf[1] = 'I'; buf[2] = 'L'; buf[3] = 'E';
    p16(buf, 4, 0x30);
    p16(buf, 6, usa_n);
    p16(buf, 16, seq);
    p16(buf, 18, links);
    u32 attrs = (u32)al8(0x30 + 2 * usa_n);
    p16(buf, 20, attrs);
    p16(buf, 22, flags);
    p32(buf, 28, v->rec);
    p32(buf, 44, recno);
    r->used = attrs;
    r->next_id = 0;
}

/* Resident: Attribut mit Wert */
static void add_res(Rec *r, u32 type, const char *name, u16 aflags, const void *val, u32 vlen, u8 indexed) {
    u8 *b = r->b + r->used;
    u32 nl = name ? (u32)slen(name) : 0;
    u32 voff = (u32)al8(24 + 2 * nl);
    u32 alen = (u32)al8(voff + vlen);
    mset(b, 0, alen);
    p32(b, 0, type);
    p32(b, 4, alen);
    b[8] = 0;
    b[9] = (u8)nl;
    p16(b, 10, nl ? 24 : 0);
    p16(b, 12, aflags);
    p16(b, 14, r->next_id++);
    p32(b, 16, vlen);
    p16(b, 20, voff);
    b[22] = indexed;
    for (u32 i = 0; i < nl; i++) p16(b, 24 + 2 * i, (u8)name[i]);
    if (vlen) mcpy(b + voff, val, vlen);
    r->used += alen;
}

/* Laufliste (mapping pairs) kodieren, liefert Laenge inklusive abschliessender 0 */
static u32 enc_runs(u8 *out, const Run *runs, int n) {
    u32 o = 0;
    i64 prev = 0;
    for (int i = 0; i < n; i++) {
        u64 len = runs[i].len;
        int lb = 1;
        while (lb < 8 && (len >> (8 * lb))) lb++;
        if ((len >> (8 * lb - 1)) & 1) lb++;   /* Laengen werden vorzeichenbehaftet gelesen */
        int ob = 0;
        i64 delta = 0;
        if (runs[i].lcn != NOLCN) {
            delta = (i64)runs[i].lcn - prev;
            prev = (i64)runs[i].lcn;
            ob = 1;
            while (ob < 8) {
                i64 lim = (i64)1 << (8 * ob - 1);
                if (delta >= -lim && delta < lim) break;
                ob++;
            }
        }
        out[o++] = (u8)((ob << 4) | lb);
        for (int k = 0; k < lb; k++) out[o++] = (u8)(len >> (8 * k));
        for (int k = 0; k < ob; k++) out[o++] = (u8)((u64)delta >> (8 * k));
    }
    out[o++] = 0;
    return o;
}

/* Nicht resident: Attribut mit Laufliste */
static void add_nr(Vol *v, Rec *r, u32 type, const char *name, const Run *runs, int nruns, u64 alloc, u64 real, u64 init) {
    u8 *b = r->b + r->used;
    u32 nl = name ? (u32)slen(name) : 0;
    u8 mp[96];
    u32 mpl = enc_runs(mp, runs, nruns);
    u32 roff = 64 + (u32)al8(2 * nl);
    u32 alen = (u32)al8(roff + mpl);
    mset(b, 0, alen);
    p32(b, 0, type);
    p32(b, 4, alen);
    b[8] = 1;
    b[9] = (u8)nl;
    p16(b, 10, nl ? 64 : 0);
    p16(b, 14, r->next_id++);
    p64(b, 16, 0);
    p64(b, 24, alloc / v->cs - 1);
    p16(b, 32, roff);
    p64(b, 40, alloc);
    p64(b, 48, real);
    p64(b, 56, init);
    for (u32 i = 0; i < nl; i++) p16(b, 64 + 2 * i, (u8)name[i]);
    mcpy(b + roff, mp, mpl);
    r->used += alen;
}

static void rec_end(Rec *r) {
    p32(r->b, r->used, 0xFFFFFFFFu);
    r->used += 8;
    p32(r->b, 24, r->used);
    p16(r->b, 40, r->next_id);
}

/* Update-Sequence-Array: die letzten zwei Bytes jedes 512-Byte-Abschnitts durch die Seriennummer ersetzen */
static void fixup(u8 *b, u32 size, u16 usn) {
    u32 uo = b[4] | ((u32)b[5] << 8);
    u32 n = size / 512;
    p16(b, uo, usn);
    for (u32 i = 1; i <= n; i++) {
        u32 p = i * 512 - 2;
        b[uo + 2 * i] = b[p];
        b[uo + 2 * i + 1] = b[p + 1];
        p16(b, p, usn);
    }
}

/* $STANDARD_INFORMATION: 48 oder 72 Bytes */
static void add_std(Vol *v, Rec *r, u32 fileflags, int big, u32 secid) {
    u8 s[72];
    mset(s, 0, 72);
    for (int i = 0; i < 4; i++) p64(s, 8 * i, v->t);
    p32(s, 32, fileflags);
    if (big) p32(s, 52, secid);
    add_res(r, 0x10, 0, 0, s, big ? 72 : 48, 0);
}

/* $FILE_NAME-Wert; liefert die Laenge */
static u32 make_fn(Vol *v, u8 *o, u64 parent, const char *name, u32 flags, u64 alloc, u64 real) {
    u32 nl = (u32)slen(name);
    mset(o, 0, 66 + 2 * nl);
    p64(o, 0, parent);
    for (int i = 0; i < 4; i++) p64(o, 8 + 8 * i, v->t);
    p64(o, 40, alloc);
    p64(o, 48, real);
    p32(o, 56, flags);
    o[64] = (u8)nl;
    o[65] = 3;   /* Win32 und DOS */
    for (u32 i = 0; i < nl; i++) p16(o, 66 + 2 * i, (u8)name[i]);
    return 66 + 2 * nl;
}

static void set_bits(u8 *bm, u64 first, u64 count) {
    for (u64 i = first; i < first + count; i++) bm[i / 8] |= (u8)(1u << (i % 8));
}

static char up1(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int name_cmp(const char *a, const char *b) {   /* Dateinamen-Kollation: ohne Beachtung der Gross-/Kleinschreibung */
    while (*a && up1(*a) == up1(*b)) {
        a++;
        b++;
    }
    return (u8)up1(*a) - (u8)up1(*b);
}

#define PARENT_ROOT (5ULL | (5ULL << 48))

typedef struct {
    const char *name;
    u32 recno;
    u16 seq;
    u8 fn[160];
    u32 fnlen;
} Ent;

static int mkfs_ntfs_impl(MkDev *d, const char *label, u8 *mem);

int mkfs_ntfs(MkDev *d, const char *label) {
    u32 bps = d->sector_size;
    if (bps != 512 && bps != 4096) return MKFS_EPARAM;
    if (d->sectors * bps < 32ULL * 1024 * 1024) return MKFS_ETOOSMALL;
    return mkfs_ntfs_impl(d, label, 0);
}

static int mkfs_ntfs_impl(MkDev *d, const char *label, u8 *unused) {
    (void)unused;
    Vol V;
    Vol *v = &V;
    v->d = d;
    v->bps = d->sector_size;
    v->spc = v->bps >= 4096 ? 1 : 8;
    v->cs = v->bps * v->spc;
    v->rec = v->bps >= 4096 ? 4096 : 1024;
    v->nclusters = (d->sectors - 1) / v->spc;   /* der letzte Sektor ist der Sicherungs-Bootsektor */
    v->t = d->ntfs_time;
    u32 cs = v->cs, rec = v->rec;
    u64 ncl = v->nclusters;
    const u32 NREC = 24;   /* MFT-Eintraege: 0-23 */

    /* ---- Layout ---- */
    u64 mft_lcn = cdiv(16384, cs);
    u64 mftc = cdiv((u64)NREC * rec, cs);
    u64 zone = ncl / 8;
    u64 data = mft_lcn + zone;
    if (data < mft_lcn + mftc + 1) data = mft_lcn + mftc + 1;
    u64 rsdc = cdiv(sizeof(T_SD_ROOT), cs);
    u64 bm_bytes = (ncl + 63) / 64 * 8;
    u64 bmc = cdiv(bm_bytes, cs);
    u64 sds_real = 0x40000 + sizeof(T_SDS_ENTRY);
    u64 sdsc = cdiv(sds_real, cs);
    u64 upc = cdiv(131072, cs);
    u64 root_sd_lcn = data;
    u64 indx_lcn = root_sd_lcn + rsdc;
    u64 attrdef_lcn = indx_lcn + 1;
    u64 bitmap_lcn = attrdef_lcn + 1;
    u64 sds_lcn = bitmap_lcn + bmc;
    u64 up_lcn = sds_lcn + sdsc;
    u64 data_end = up_lcn + upc;
    u64 mirc = cdiv(4ULL * rec, cs);
    u64 mirror_lcn = ncl / 2;
    u64 lf_bytes = d->sectors * v->bps < 256ULL * 1024 * 1024 ? 2ULL << 20 : (d->sectors * v->bps < 4ULL << 30 ? 8ULL << 20 : 32ULL << 20);
    u64 lfc = cdiv(lf_bytes, cs);
    u64 lf_lcn = mirror_lcn + mirc;
    if (data_end >= mirror_lcn || lf_lcn + lfc >= ncl) return MKFS_ETOOSMALL;
    u64 mft_alloc = mftc * cs, mft_real = (u64)NREC * rec;

    /* ---- Bitmap der belegten Cluster ---- */
    u8 *bm = d->alloc((size_t)(bmc * cs));
    u8 *recs = d->alloc((size_t)(mftc * cs));
    if (!bm || !recs) {
        if (bm) d->release(bm);
        if (recs) d->release(recs);
        return MKFS_ENOMEM;
    }
    mset(bm, 0, (size_t)(bmc * cs));
    mset(recs, 0, (size_t)(mftc * cs));
    set_bits(bm, 0, 2 + 1);                  /* $Boot, $MFT-Bitmap */
    set_bits(bm, mft_lcn, mftc);
    set_bits(bm, data, data_end - data);
    set_bits(bm, mirror_lcn, mirc);
    set_bits(bm, lf_lcn, lfc);
    for (u64 i = ncl; i < bm_bytes * 8; i++) bm[i / 8] |= (u8)(1u << (i % 8));   /* Rest hinter dem Volume = belegt */

    /* ---- Namen der Wurzel-Eintraege (FILE_NAME-Werte werden fuer den Index gebraucht) ---- */
    Ent ents[12];
    int ne = 0;
    u8 fnv[160];
    u32 fnl;
    Rec r;
    u8 *rb;
    u16 seqs[24];
    for (u32 i = 0; i < NREC; i++) seqs[i] = i == 0 ? 1 : (u16)i;

    #define ADD_ENT(NAME, RECNO, FLAGS, ALLOC, REAL) do { \
        fnl = make_fn(v, fnv, PARENT_ROOT, NAME, FLAGS, ALLOC, REAL); \
        ents[ne].name = NAME; ents[ne].recno = RECNO; ents[ne].seq = seqs[RECNO]; ents[ne].fnlen = fnl; \
        mcpy(ents[ne].fn, fnv, fnl); ne++; } while (0)
    #define REC_FN() add_res(&r, 0x30, 0, 0, fnv, fnl, 1)

    /* 0: $MFT */
    rb = recs + 0 * rec;
    rec_begin(v, &r, rb, 0, seqs[0], 1, 1);
    add_std(v, &r, 6, 1, 0x100);
    ADD_ENT("$MFT", 0, 6, mft_alloc, mft_real);
    REC_FN();
    { Run rn[1] = {{mft_lcn, mftc}}; add_nr(v, &r, 0x80, 0, rn, 1, mft_alloc, mft_real, mft_real); }
    { Run rn[1] = {{2, 1}}; add_nr(v, &r, 0xB0, 0, rn, 1, cs, 8, 8); }
    rec_end(&r);

    /* 1: $MFTMirr */
    rb = recs + 1 * rec;
    rec_begin(v, &r, rb, 1, seqs[1], 1, 1);
    add_std(v, &r, 6, 1, 0x100);
    ADD_ENT("$MFTMirr", 1, 6, mirc * cs, 4ULL * rec);
    REC_FN();
    { Run rn[1] = {{mirror_lcn, mirc}}; add_nr(v, &r, 0x80, 0, rn, 1, mirc * cs, 4ULL * rec, 4ULL * rec); }
    rec_end(&r);

    /* 2: $LogFile */
    rb = recs + 2 * rec;
    rec_begin(v, &r, rb, 2, seqs[2], 1, 1);
    add_std(v, &r, 6, 1, 0x100);
    ADD_ENT("$LogFile", 2, 6, lfc * cs, lfc * cs);
    REC_FN();
    { Run rn[1] = {{lf_lcn, lfc}}; add_nr(v, &r, 0x80, 0, rn, 1, lfc * cs, lfc * cs, lfc * cs); }
    rec_end(&r);

    /* 3: $Volume */
    rb = recs + 3 * rec;
    rec_begin(v, &r, rb, 3, seqs[3], 1, 1);
    add_std(v, &r, 6, 0, 0);
    ADD_ENT("$Volume", 3, 6, 0, 0);
    REC_FN();
    add_res(&r, 0x50, 0, 0, T_SD_A, sizeof(T_SD_A), 0);
    {
        u8 nm[64];
        u32 nl = 0;
        for (; label && label[nl] && nl < 30; nl++) p16(nm, 2 * nl, (u8)label[nl]);
        add_res(&r, 0x60, 0, 0, nm, 2 * nl, 0);
        u8 vi[12];
        mset(vi, 0, 12);
        vi[8] = 3;   /* NTFS 3.1 */
        vi[9] = 1;
        add_res(&r, 0x70, 0, 0, vi, 12, 0);
    }
    add_res(&r, 0x80, 0, 0, 0, 0, 0);
    rec_end(&r);

    /* 4: $AttrDef */
    rb = recs + 4 * rec;
    rec_begin(v, &r, rb, 4, seqs[4], 1, 1);
    add_std(v, &r, 6, 0, 0);
    ADD_ENT("$AttrDef", 4, 6, cs, sizeof(T_ATTRDEF));
    REC_FN();
    add_res(&r, 0x50, 0, 0, T_SD_B, sizeof(T_SD_B), 0);
    { Run rn[1] = {{attrdef_lcn, 1}}; add_nr(v, &r, 0x80, 0, rn, 1, cs, sizeof(T_ATTRDEF), sizeof(T_ATTRDEF)); }
    rec_end(&r);

    /* 5: Wurzelverzeichnis */
    rb = recs + 5 * rec;
    rec_begin(v, &r, rb, 5, seqs[5], 3, 1);
    add_std(v, &r, 0x26, 0, 0);
    ADD_ENT(".", 5, 0x10000006, 0, 0);
    REC_FN();
    { Run rn[1] = {{root_sd_lcn, rsdc}}; add_nr(v, &r, 0x50, 0, rn, 1, rsdc * cs, sizeof(T_SD_ROOT), sizeof(T_SD_ROOT)); }
    {
        u8 ir[56];
        mset(ir, 0, 56);
        p32(ir, 0, 0x30);                         /* indiziertes Attribut: $FILE_NAME */
        p32(ir, 4, 1);                            /* Kollation: Dateinamen */
        p32(ir, 8, 4096);                         /* Indexblock-Groesse */
        ir[12] = (u8)(4096 / cs);                 /* Cluster je Indexblock */
        p32(ir, 16, 0x10);                        /* Knoten: Eintraege ab */
        p32(ir, 20, 0x28);                        /* Laenge der Eintraege */
        p32(ir, 24, 0x28);
        ir[28] = 1;                               /* grosser Index (Unterknoten) */
        p16(ir, 32 + 8, 0x18);                    /* Endeintrag: Laenge 0x18 */
        p16(ir, 32 + 12, 3);                      /* Ende + Unterknoten */
        add_res(&r, 0x90, "$I30", 0, ir, 56, 0);
    }
    { Run rn[1] = {{indx_lcn, 1}}; add_nr(v, &r, 0xA0, "$I30", rn, 1, cs, 4096, 4096); }
    { u8 one[8] = {1, 0, 0, 0, 0, 0, 0, 0}; add_res(&r, 0xB0, "$I30", 0, one, 8, 0); }
    rec_end(&r);

    /* 6: $Bitmap */
    rb = recs + 6 * rec;
    rec_begin(v, &r, rb, 6, seqs[6], 1, 1);
    add_std(v, &r, 6, 1, 0x100);
    ADD_ENT("$Bitmap", 6, 6, bmc * cs, bm_bytes);
    REC_FN();
    { Run rn[1] = {{bitmap_lcn, bmc}}; add_nr(v, &r, 0x80, 0, rn, 1, bmc * cs, bm_bytes, bm_bytes); }
    rec_end(&r);

    /* 7: $Boot */
    rb = recs + 7 * rec;
    rec_begin(v, &r, rb, 7, seqs[7], 1, 1);
    add_std(v, &r, 6, 0, 0);
    ADD_ENT("$Boot", 7, 6, 8192, 8192);
    REC_FN();
    add_res(&r, 0x50, 0, 0, T_SD_B, sizeof(T_SD_B), 0);
    { Run rn[1] = {{0, cdiv(8192, cs)}}; add_nr(v, &r, 0x80, 0, rn, 1, cdiv(8192, cs) * cs, 8192, 8192); }
    rec_end(&r);

    /* 8: $BadClus (unbenannter Strom leer, $Bad komplett als Loch) */
    rb = recs + 8 * rec;
    rec_begin(v, &r, rb, 8, seqs[8], 1, 1);
    add_std(v, &r, 6, 1, 0x100);
    ADD_ENT("$BadClus", 8, 6, 0, 0);
    REC_FN();
    add_res(&r, 0x80, 0, 0, 0, 0, 0);
    { Run rn[1] = {{NOLCN, ncl}}; add_nr(v, &r, 0x80, "$Bad", rn, 1, ncl * cs, ncl * cs, 0); }
    rec_end(&r);

    /* 9: $Secure (Sicherheitsdeskriptoren: $SDS-Strom und die Indexe $SDH/$SII) */
    rb = recs + 9 * rec;
    rec_begin(v, &r, rb, 9, seqs[9], 9, 1);
    add_std(v, &r, 0x20000006, 1, 0x101);
    ADD_ENT("$Secure", 9, 0x20000006, 0, 0);
    REC_FN();
    { Run rn[1] = {{sds_lcn, sdsc}}; add_nr(v, &r, 0x80, "$SDS", rn, 1, sdsc * cs, sds_real, sds_real); }
    add_res(&r, 0x90, "$SDH", 0, T_SDH_ROOT, sizeof(T_SDH_ROOT), 0);
    add_res(&r, 0x90, "$SII", 0, T_SII_ROOT, sizeof(T_SII_ROOT), 0);
    rec_end(&r);

    /* 10: $UpCase */
    rb = recs + 10 * rec;
    rec_begin(v, &r, rb, 10, seqs[10], 1, 1);
    add_std(v, &r, 6, 1, 0x100);
    ADD_ENT("$UpCase", 10, 6, upc * cs, 131072);
    REC_FN();
    { Run rn[1] = {{up_lcn, upc}}; add_nr(v, &r, 0x80, 0, rn, 1, upc * cs, 131072, 131072); }
    add_res(&r, 0x80, "$Info", 0, T_UPCASE_INFO, sizeof(T_UPCASE_INFO), 0);
    rec_end(&r);

    /* 11: $Extend (leeres Verzeichnis) */
    rb = recs + 11 * rec;
    rec_begin(v, &r, rb, 11, seqs[11], 3, 1);
    add_std(v, &r, 6, 1, 0x101);
    ADD_ENT("$Extend", 11, 0x10000006, 0, 0);
    REC_FN();
    {
        u8 ir[48];
        mset(ir, 0, 48);
        p32(ir, 0, 0x30);
        p32(ir, 4, 1);
        p32(ir, 8, 4096);
        ir[12] = (u8)(4096 / cs);
        p32(ir, 16, 0x10);
        p32(ir, 20, 0x20);
        p32(ir, 24, 0x20);
        ir[28] = 0;                       /* kleiner Index: nur der Endeintrag */
        p16(ir, 32 + 8, 0x10);
        p16(ir, 32 + 12, 2);
        add_res(&r, 0x90, "$I30", 0, ir, 48, 0);
    }
    rec_end(&r);

    /* 12-15: reservierte, belegte Eintraege; 16-23: frei */
    for (u32 n = 12; n < NREC; n++) {
        rb = recs + (u64)n * rec;
        if (n < 16) {
            rec_begin(v, &r, rb, n, seqs[n], 1, 0);
            add_std(v, &r, 6, 0, 0);
            add_res(&r, 0x50, 0, 0, T_SD_A, sizeof(T_SD_A), 0);
            add_res(&r, 0x80, 0, 0, 0, 0, 0);
        } else {
            rec_begin(v, &r, rb, n, seqs[n], 0, 0);
            add_std(v, &r, 6, 0, 0);
        }
        rec_end(&r);
    }

    /* ---- Wurzel-Indexblock (INDX) nach Namen sortiert ---- */
    for (int i = 1; i < ne; i++) {
        Ent t = ents[i];
        int j = i;
        while (j > 0 && name_cmp(t.name, ents[j - 1].name) < 0) {
            ents[j] = ents[j - 1];
            j--;
        }
        ents[j] = t;
    }
    u8 *ix = d->alloc(4096);
    if (!ix) {
        d->release(bm);
        d->release(recs);
        return MKFS_ENOMEM;
    }
    mset(ix, 0, 4096);
    ix[0] = 'I'; ix[1] = 'N'; ix[2] = 'D'; ix[3] = 'X';
    p16(ix, 4, 0x28);
    p16(ix, 6, 9);
    u32 eo = 0x40;
    for (int i = 0; i < ne; i++) {
        u32 el = (u32)al8(16 + ents[i].fnlen);
        p64(ix, eo, (u64)ents[i].recno | ((u64)ents[i].seq << 48));
        p16(ix, eo + 8, el);
        p16(ix, eo + 10, ents[i].fnlen);
        p16(ix, eo + 12, 0);
        mcpy(ix + eo + 16, ents[i].fn, ents[i].fnlen);
        eo += el;
    }
    p16(ix, eo + 8, 16);   /* Endeintrag */
    p16(ix, eo + 12, 2);
    eo += 16;
    p32(ix, 0x18, 0x28);               /* Eintraege ab */
    p32(ix, 0x1C, eo - 0x18);          /* Laenge */
    p32(ix, 0x20, 4096 - 0x18);        /* belegt */
    ix[0x24] = 0;                      /* Blatt */
    fixup(ix, 4096, 1);

    /* ---- Fixups der MFT-Eintraege und Schreiben ---- */
    for (u32 n = 0; n < NREC; n++) fixup(recs + (u64)n * rec, rec, 1);

    int rr = 0;
    progress(d, "boot", 5);
    /* $Boot: Bootsektor aus der Vorlage, Felder anpassen */
    {
        u8 *bs = d->alloc(8192 > cs ? 8192 : cs);
        if (!bs) rr = MKFS_ENOMEM;
        else {
            mset(bs, 0, 8192);
            mcpy(bs, T_BOOT, 512);
            p16(bs, 11, v->bps);
            bs[13] = (u8)v->spc;
            p32(bs, 0x1C, (u32)d->start_lba);
            p64(bs, 0x28, d->sectors - 1);
            p64(bs, 0x30, mft_lcn);
            p64(bs, 0x38, mirror_lcn);
            bs[0x40] = rec >= cs ? (u8)(rec / cs) : (u8)(rec == 1024 ? 0xF6 : 0xF4);
            bs[0x44] = (u8)(4096 / cs);
            p64(bs, 0x48, d->serial);
            p32(bs, 0x50, 0);
            bs[510] = 0x55;
            bs[511] = 0xAA;
            rr = wr(d, 0, bs, 8192);
            if (!rr) rr = wr(d, (d->sectors - 1) * v->bps, bs, 512);   /* Sicherung im letzten Sektor */
            d->release(bs);
        }
    }
    if (!rr) rr = wr(d, 2 * (u64)cs, (const u8[8]){0xFF, 0xFF, 0, 0, 0, 0, 0, 0}, 8);   /* MFT-Bitmap: Eintraege 0-15 belegt */
    progress(d, "mft", 15);
    if (!rr) rr = wr(d, mft_lcn * cs, recs, mftc * cs);
    if (!rr) rr = wr(d, mirror_lcn * cs, recs, 4ULL * rec);
    if (!rr) rr = wr(d, root_sd_lcn * cs, T_SD_ROOT, sizeof(T_SD_ROOT));
    if (!rr) rr = wr(d, indx_lcn * cs, ix, 4096);
    if (!rr) rr = wr(d, attrdef_lcn * cs, T_ATTRDEF, sizeof(T_ATTRDEF));
    if (!rr) rr = wr(d, bitmap_lcn * cs, bm, bmc * cs);
    progress(d, "secure", 30);
    if (!rr) {   /* $SDS: Eintrag bei 0 und gespiegelt bei 0x40000 */
        u8 *sds = d->alloc((size_t)(sdsc * cs));
        if (!sds) rr = MKFS_ENOMEM;
        else {
            mset(sds, 0, (size_t)(sdsc * cs));
            mcpy(sds, T_SDS_ENTRY, sizeof(T_SDS_ENTRY));
            mcpy(sds + 0x40000, T_SDS_ENTRY, sizeof(T_SDS_ENTRY));
            rr = wr(d, sds_lcn * cs, sds, sdsc * cs);
            d->release(sds);
        }
    }
    progress(d, "upcase", 40);
    if (!rr) {   /* $UpCase-Tabelle aus der Laufliste aufbauen */
        u8 *up = d->alloc((size_t)(upc * cs));
        if (!up) rr = MKFS_ENOMEM;
        else {
            mset(up, 0, (size_t)(upc * cs));
            for (u32 c = 0; c < 65536; c++) p16(up, 2 * c, c);
            for (size_t i = 0; i < sizeof(T_UPCASE_RUNS) / sizeof(T_UPCASE_RUNS[0]); i++)
                for (u32 k = 0; k < T_UPCASE_RUNS[i].count; k++) {
                    u32 c = T_UPCASE_RUNS[i].start + k * T_UPCASE_RUNS[i].step;
                    p16(up, 2 * c, (u32)((int)c + T_UPCASE_RUNS[i].delta));
                }
            rr = wr(d, up_lcn * cs, up, 131072);
            d->release(up);
        }
    }
    progress(d, "logfile", 50);
    if (!rr) rr = fill(d, lf_lcn * cs, lfc * cs, 0xFF);   /* $LogFile: leer */
    progress(d, "done", 100);
    d->release(ix);
    d->release(bm);
    d->release(recs);
    return rr;
}

/* ------------------------------------------------------------------ */
/* FAT32                                                               */
/* ------------------------------------------------------------------ */
int mkfs_fat32(MkDev *d, const char *label) {
    u32 bps = d->sector_size;
    if (bps != 512 && bps != 4096) return MKFS_EPARAM;
    u64 sectors = d->sectors;
    u64 bytes = sectors * bps;
    u32 spc;
    if (bytes <= 260ULL << 20) spc = 1;
    else if (bytes <= 8ULL << 30) spc = 8;
    else if (bytes <= 16ULL << 30) spc = 16;
    else if (bytes <= 32ULL << 30) spc = 32;
    else spc = 64;
    if (bps == 4096) spc = spc > 1 ? spc / 8 : 1;   /* 4096-Byte-Sektoren: Cluster entsprechend kleiner (mind. 1 Sektor) */
    if (spc == 0) spc = 1;
    const u32 reserved = 32;
    u64 fat = 1, clusters = 0;
    for (int i = 0; i < 4; i++) {
        u64 data = sectors - reserved - 2 * fat;
        clusters = data / spc;
        fat = cdiv((clusters + 2) * 4, bps);
    }
    if (clusters < 65525) return MKFS_ETOOSMALL;   /* sonst waere es kein FAT32 */
    if (clusters > 0x0FFFFFF0ULL) return MKFS_EPARAM;
    u32 cl_bytes = spc * bps;

    u8 *bs = d->alloc(bps);
    if (!bs) return MKFS_ENOMEM;
    mset(bs, 0, bps);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    mcpy(bs + 3, "BIYOS   ", 8);
    p16(bs, 11, bps);
    bs[13] = (u8)spc;
    p16(bs, 14, reserved);
    bs[16] = 2;                         /* zwei FATs */
    bs[21] = 0xF8;
    p16(bs, 24, 63);
    p16(bs, 26, 255);
    p32(bs, 28, (u32)d->start_lba);
    p32(bs, 32, (u32)sectors);
    p32(bs, 36, (u32)fat);
    p32(bs, 44, 2);                     /* Wurzelverzeichnis: Cluster 2 */
    p16(bs, 48, 1);                     /* FSInfo */
    p16(bs, 50, 6);                     /* Sicherungs-Bootsektor */
    bs[64] = 0x80;
    bs[66] = 0x29;
    p32(bs, 67, (u32)d->serial);
    char lab[11];
    mset(lab, ' ', 11);
    for (int i = 0; label && label[i] && i < 11; i++) lab[i] = up1(label[i]);
    mcpy(bs + 71, lab, 11);
    mcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55;
    bs[511] = 0xAA;

    u8 *fi = d->alloc(bps);
    u8 *ft = d->alloc(bps);
    if (!fi || !ft) {
        d->release(bs);
        if (fi) d->release(fi);
        if (ft) d->release(ft);
        return MKFS_ENOMEM;
    }
    mset(fi, 0, bps);
    p32(fi, 0, 0x41615252);
    p32(fi, 484, 0x61417272);
    p32(fi, 488, (u32)(clusters - 1));   /* freie Cluster (Cluster 2 ist das Wurzelverzeichnis) */
    p32(fi, 492, 3);
    p32(fi, 508, 0xAA550000);
    mset(ft, 0, bps);
    p32(ft, 0, 0x0FFFFFF8);
    p32(ft, 4, 0x0FFFFFFF);
    p32(ft, 8, 0x0FFFFFFF);

    int r = 0;
    progress(d, "boot", 10);
    r = wr(d, 0, bs, bps);
    if (!r) r = wr(d, 1ULL * bps, fi, bps);
    if (!r) r = wr(d, 6ULL * bps, bs, bps);
    if (!r) r = wr(d, 7ULL * bps, fi, bps);
    progress(d, "fat", 30);
    for (int k = 0; k < 2 && !r; k++) {
        u64 base = (reserved + (u64)k * fat) * bps;
        r = fill(d, base, fat * bps, 0);
        if (!r) r = wr(d, base, ft, bps);
    }
    progress(d, "root", 90);
    if (!r) {   /* Wurzelverzeichnis: leer, mit Datentraegername */
        u8 *rt = d->alloc(cl_bytes);
        if (!rt) r = MKFS_ENOMEM;
        else {
            mset(rt, 0, cl_bytes);
            mcpy(rt, lab, 11);
            rt[11] = 0x08;
            r = wr(d, (reserved + 2 * fat) * bps, rt, cl_bytes);
            d->release(rt);
        }
    }
    progress(d, "done", 100);
    d->release(bs);
    d->release(fi);
    d->release(ft);
    return r;
}

/* ------------------------------------------------------------------ */
/* GPT                                                                 */
/* ------------------------------------------------------------------ */
/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B, die ersten drei Felder little-endian */
const uint8_t MK_GUID_ESP[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
const uint8_t MK_GUID_BASIC[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};

uint64_t mkfs_align_mib(uint64_t lba, uint32_t ss) {
    u64 a = (1ULL << 20) / ss;
    return (lba + a - 1) / a * a;
}

static u32 crc32(const u8 *b, size_t n) {
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (u32)-(int)(c & 1));
    }
    return ~c;
}

int mkfs_gpt(MkDev *disk, const MkPart *parts, int nparts, const uint8_t disk_guid[16]) {
    u32 ss = disk->sector_size;
    if (ss != 512 && ss != 4096) return MKFS_EPARAM;
    if (nparts < 0 || nparts > 128) return MKFS_EPARAM;
    u64 total = disk->sectors, last = total - 1;
    u32 ent_sec = 16384 / ss;
    u64 first_usable = 2 + ent_sec, last_usable = last - ent_sec - 1;
    if (total < 2 + 2 * (u64)ent_sec + 2) return MKFS_ETOOSMALL;

    u8 *ents = disk->alloc(16384);
    u8 *sec = disk->alloc(ss);
    if (!ents || !sec) {
        if (ents) disk->release(ents);
        if (sec) disk->release(sec);
        return MKFS_ENOMEM;
    }
    mset(ents, 0, 16384);
    for (int i = 0; i < nparts; i++) {
        u8 *e = ents + 128 * i;
        if (parts[i].first_lba < first_usable || parts[i].first_lba + parts[i].sectors - 1 > last_usable) {
            disk->release(ents);
            disk->release(sec);
            return MKFS_EPARAM;
        }
        mcpy(e, parts[i].type_guid, 16);
        mcpy(e + 16, parts[i].guid, 16);
        p64(e, 32, parts[i].first_lba);
        p64(e, 40, parts[i].first_lba + parts[i].sectors - 1);
        for (int k = 0; k < 35 && parts[i].name[k]; k++) p16(e, 56 + 2 * k, (u8)parts[i].name[k]);
    }
    u32 ecrc = crc32(ents, 16384);

    int r = 0;
    /* Schutz-MBR */
    mset(sec, 0, ss);
    sec[446 + 2] = 2;   /* CHS-Anfang (nur der Form halber) */
    sec[446 + 4] = 0xEE;
    sec[446 + 5] = 0xFF; sec[446 + 6] = 0xFF; sec[446 + 7] = 0xFF;
    p32(sec, 446 + 8, 1);
    p32(sec, 446 + 12, total - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (u32)(total - 1));
    sec[510] = 0x55;
    sec[511] = 0xAA;
    r = wr(disk, 0, sec, ss);

    for (int which = 0; which < 2 && !r; which++) {   /* 0 = Haupt-, 1 = Sicherungs-Tabelle */
        u64 hdr_lba = which ? last : 1;
        u64 other = which ? 1 : last;
        u64 ent_lba = which ? last - ent_sec : 2;
        mset(sec, 0, ss);
        mcpy(sec, "EFI PART", 8);
        p32(sec, 8, 0x00010000);
        p32(sec, 12, 92);
        p64(sec, 24, hdr_lba);
        p64(sec, 32, other);
        p64(sec, 40, first_usable);
        p64(sec, 48, last_usable);
        mcpy(sec + 56, disk_guid, 16);
        p64(sec, 72, ent_lba);
        p32(sec, 80, 128);
        p32(sec, 84, 128);
        p32(sec, 88, ecrc);
        p32(sec, 16, crc32(sec, 92));
        r = wr(disk, ent_lba * ss, ents, 16384);
        if (!r) r = wr(disk, hdr_lba * ss, sec, ss);
    }
    disk->release(ents);
    disk->release(sec);
    return r;
}
