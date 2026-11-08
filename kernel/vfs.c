/* SPDX-License-Identifier: MIT */
/*
 * vfs.c - virtuelle Dateisystem-Schicht: Laufwerksbuchstaben, aktuelles Verzeichnis, Pfade
 *
 * Pfade duerfen '/' oder '\\' benutzen, optional mit Laufwerk ("E:\\dir\\datei"). Relative
 * Pfade gelten ab dem aktuellen Verzeichnis des jeweiligen Laufwerks (wie bei DOS). Die
 * Dateisysteme bekommen normalisierte absolute Pfade ("/dir/datei").
 *
 * Automatisch eingebundene Laufwerke sind schreibgeschuetzt. Schreiben muss man mit
 * "mount <geraet> <buchstabe> rw" erlauben (Schutz fuer echte Datentraeger).
 */
#include "kernel.h"

void *memset(void *d, int c, size_t n);

static Fs *mounts[26];
static char cwd[26][256];
static char cur;   /* aktuelles Laufwerk, 0 = keins */

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

const char *fs_strerror(int err) {
    switch (err) {
    case FS_ENOENT: return "NICHT GEFUNDEN";
    case FS_EEXIST: return "EXISTIERT SCHON";
    case FS_ENOTDIR: return "KEIN VERZEICHNIS";
    case FS_EISDIR: return "IST EIN VERZEICHNIS";
    case FS_ENOSPC: return "KEIN PLATZ";
    case FS_EIO: return "LESE-/SCHREIBFEHLER";
    case FS_EINVAL: return "UNGUELTIGE ANGABE";
    case FS_ENOTEMPTY: return "VERZEICHNIS NICHT LEER";
    case FS_ENAME: return "UNGUELTIGER NAME";
    case FS_EROFS: return "SCHREIBGESCHUETZT (MOUNT <GERAET> <LAUFWERK> RW)";
    case FS_ENODRV: return "LAUFWERK NICHT EINGEBUNDEN";
    case FS_ENOSYS: return "NICHT UNTERSTUETZT";
    case FS_EDIRTY: return "VOLUME NICHT SAUBER (WINDOWS: SCHNELLSTART AUS, CHKDSK)";
    default: return "FEHLER";
    }
}

/* ------------------------------------------------------------------ */
int vfs_mount(char letter, Fs *fs) {
    letter = up(letter);
    if (letter < 'A' || letter > 'Z') return FS_EINVAL;
    if (mounts[letter - 'A']) return FS_EEXIST;
    mounts[letter - 'A'] = fs;
    cwd[letter - 'A'][0] = '/';
    cwd[letter - 'A'][1] = 0;
    if (!cur) cur = letter;
    return 0;
}

int vfs_unmount(char letter) {
    letter = up(letter);
    if (letter < 'A' || letter > 'Z' || !mounts[letter - 'A']) return FS_ENODRV;
    mounts[letter - 'A'] = 0;
    if (cur == letter) {
        cur = 0;
        for (int i = 0; i < 26; i++)
            if (mounts[i]) {
                cur = (char)('A' + i);
                break;
            }
    }
    return 0;
}

Fs *vfs_get(char letter) {
    letter = up(letter);
    return (letter >= 'A' && letter <= 'Z') ? mounts[letter - 'A'] : 0;
}

char vfs_cur_drive(void) { return cur; }
const char *vfs_cur_path(void) { return cur ? cwd[cur - 'A'] : "/"; }

int vfs_set_drive(char letter) {
    letter = up(letter);
    if (!vfs_get(letter)) return FS_ENODRV;
    cur = letter;
    return 0;
}

/* Eingabepfad -> Dateisystem, normalisierter Pfad (out >= 512 Zeichen) und Laufwerk */
static int vres(const char *in, Fs **fsout, char *out, char *drive_out) {
    char drive = cur;
    const char *p = in;
    if (p[0] && p[1] == ':') {
        drive = up(p[0]);
        p += 2;
    }
    if (drive < 'A' || drive > 'Z' || !mounts[drive - 'A']) return FS_ENODRV;

    char comb[1100];
    size_t n = 0;
    if (*p != '/' && *p != '\\') {   /* relativ: ab dem aktuellen Verzeichnis dieses Laufwerks */
        const char *b = cwd[drive - 'A'];
        while (*b && n < 520) comb[n++] = *b++;
        comb[n++] = '/';
    }
    while (*p && n < 1090) comb[n++] = *p++;
    comb[n] = 0;

    size_t olen = 1;
    out[0] = '/';
    size_t marks[64];
    int depth = 0;
    for (size_t i = 0; i <= n;) {
        while (i < n && (comb[i] == '/' || comb[i] == '\\')) i++;
        if (i >= n) break;
        size_t s = i;
        while (i < n && comb[i] != '/' && comb[i] != '\\') i++;
        size_t cl = i - s;
        if (cl == 1 && comb[s] == '.') continue;
        if (cl == 2 && comb[s] == '.' && comb[s + 1] == '.') {
            if (depth > 0) olen = marks[--depth];
            continue;
        }
        if (depth >= 64 || cl > 255 || olen + cl + 1 >= 510) return FS_EINVAL;
        marks[depth++] = olen;
        if (olen > 1) out[olen++] = '/';
        for (size_t k = 0; k < cl; k++) out[olen++] = comb[s + k];
    }
    out[olen] = 0;
    *fsout = mounts[drive - 'A'];
    if (drive_out) *drive_out = drive;
    return 0;
}

int vfs_cd(const char *path) {
    Fs *fs;
    char norm[512], drive;
    int r = vres(path, &fs, norm, &drive);
    if (r) return r;
    FsEntry e;
    r = fs->stat(fs, norm, &e);
    if (r) return r;
    if (!e.is_dir) return FS_ENOTDIR;
    size_t n = slen(norm);
    for (size_t i = 0; i <= n; i++) cwd[drive - 'A'][i] = norm[i];
    cur = drive;
    return 0;
}

int vfs_stat(const char *path, FsEntry *e) {
    Fs *fs;
    char norm[512];
    int r = vres(path, &fs, norm, 0);
    return r ? r : fs->stat(fs, norm, e);
}

int vfs_readdir(const char *path, FsDirCb cb, void *ctx) {
    Fs *fs;
    char norm[512];
    int r = vres(path, &fs, norm, 0);
    return r ? r : fs->readdir(fs, norm, cb, ctx);
}

int64_t vfs_read(const char *path, uint64_t off, void *buf, uint64_t len) {
    Fs *fs;
    char norm[512];
    int r = vres(path, &fs, norm, 0);
    return r ? r : fs->read(fs, norm, off, buf, len);
}

/* ganze Datei lesen; *buf per kapi.malloc (am Ende ein 0-Byte), Aufrufer gibt frei */
int vfs_read_all(const char *path, uint8_t **buf, uint64_t *len) {
    FsEntry e;
    int r = vfs_stat(path, &e);
    if (r) return r;
    if (e.is_dir) return FS_EISDIR;
    uint8_t *b = kapi.malloc((size_t)e.size + 1);
    if (!b) return FS_ENOSPC;
    int64_t n = e.size ? vfs_read(path, 0, b, e.size) : 0;
    if (n < 0) {
        kapi.free(b);
        return (int)n;
    }
    b[n] = 0;
    *buf = b;
    *len = (uint64_t)n;
    return 0;
}

int vfs_write(const char *path, const void *buf, uint64_t len, int append) {
    Fs *fs;
    char norm[512];
    int r = vres(path, &fs, norm, 0);
    if (r) return r;
    if (!fs->write) return FS_ENOSYS;
    if (fs->readonly) return FS_EROFS;
    return fs->write(fs, norm, buf, len, append);
}

int vfs_mkdir(const char *path) {
    Fs *fs;
    char norm[512];
    int r = vres(path, &fs, norm, 0);
    if (r) return r;
    if (!fs->mkdir) return FS_ENOSYS;
    if (fs->readonly) return FS_EROFS;
    return fs->mkdir(fs, norm);
}

int vfs_remove(const char *path) {
    Fs *fs;
    char norm[512];
    int r = vres(path, &fs, norm, 0);
    if (r) return r;
    if (!fs->remove) return FS_ENOSYS;
    if (fs->readonly) return FS_EROFS;
    return fs->remove(fs, norm);
}

int vfs_rename(const char *from, const char *to) {
    Fs *fs1, *fs2;
    char n1[512], n2[512];
    int r = vres(from, &fs1, n1, 0);
    if (r) return r;
    r = vres(to, &fs2, n2, 0);
    if (r) return r;
    if (fs1 != fs2) return FS_EINVAL;   /* ueber Laufwerke hinweg: copy + del */
    if (!fs1->rename) return FS_ENOSYS;
    if (fs1->readonly) return FS_EROFS;
    return fs1->rename(fs1, n1, n2);
}

int vfs_statfs(char letter, uint64_t *total, uint64_t *free_b) {
    Fs *fs = vfs_get(letter ? letter : cur);
    if (!fs) return FS_ENODRV;
    return fs->statfs(fs, total, free_b);
}

/* ------------------------------------------------------------------ */
/* Einbinden                                                           */
/* ------------------------------------------------------------------ */
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int vfs_mount_dev(const char *devname, char letter, int rw) {
    BlockDev *d = blk_find(devname);
    if (!d) return FS_ENOENT;
    Fs *fs = 0;
    int r;
    if (streq(d->fs, "FAT32")) r = fat32_mount(d, &fs);
    else if (streq(d->fs, "NTFS")) r = ntfs_mount(d, &fs);
    else return FS_ENOSYS;
    if (r) return r;
    if (rw && !fs->write) return FS_ENOSYS;
    if (rw && fs->enable_write) {
        int e = fs->enable_write(fs);
        if (e) return e;
    }
    fs->readonly = rw ? 0 : 1;
    r = vfs_mount(letter, fs);
    return r;
}

int vfs_set_rw(char letter, int rw) {
    Fs *fs = vfs_get(letter);
    if (!fs) return FS_ENODRV;
    if (rw && !fs->write) return FS_ENOSYS;
    if (rw && fs->enable_write) {
        int e = fs->enable_write(fs);
        if (e) return e;
    }
    fs->readonly = rw ? 0 : 1;
    return 0;
}

/* alle erkannten FAT32- und NTFS-Partitionen schreibgeschuetzt einbinden:
 * die erste NTFS-Partition als C:, alle anderen ab E: */
void vfs_automount(void) {
    char next = 'E';
    for (int i = 0; i < blk_count(); i++) {
        BlockDev *d = blk_get(i);
        int ntfs = streq(d->fs, "NTFS");
        if (!d->parent || (!ntfs && !streq(d->fs, "FAT32"))) continue;
        char letter;
        if (ntfs && !mounts['C' - 'A']) {
            letter = 'C';
        } else {
            while (next <= 'Z' && mounts[next - 'A']) next++;
            if (next > 'Z') break;
            letter = next;
        }
        int r = vfs_mount_dev(d->name, letter, 0);
        con_puts("MOUNT: ");
        con_putc(letter);
        con_puts(": = ");
        con_puts(d->name);
        if (r) {
            con_puts(" FEHLGESCHLAGEN: ");
            con_puts(fs_strerror(r));
        } else {
            con_puts(" (");
            con_puts(vfs_get(letter)->type);
            con_puts(", ");
            con_puts(vfs_get(letter)->label);
            con_puts(", NUR LESEN)");
            if (letter == next) next++;
        }
        con_putc('\n');
    }
}
