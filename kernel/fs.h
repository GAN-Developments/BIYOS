/*
 * fs.h - Dateisystem-Schnittstelle von BIYOS
 *
 * Jedes Dateisystem (FAT32, spaeter NTFS) liefert ein Fs mit pfadbasierten Operationen. Pfade
 * sind hier schon normalisiert: absolut, mit '/', ohne "." und ".." (das erledigt vfs.c).
 * Die Funktionszeiger sind austauschbar wie alles in BIYOS.
 *
 * Rueckgabewerte: 0 = ok, negative FS_E*-Codes bei Fehlern. read liefert die Anzahl gelesener Bytes.
 */
#ifndef BIYOS_FS_H
#define BIYOS_FS_H

#include <stdint.h>
#include "blk.h"

#define FS_OK         0
#define FS_ENOENT    -2   /* nicht gefunden */
#define FS_EEXIST    -3   /* existiert schon */
#define FS_ENOTDIR   -4
#define FS_EISDIR    -5
#define FS_ENOSPC    -6   /* kein Platz */
#define FS_EIO       -7
#define FS_EINVAL    -8
#define FS_ENOTEMPTY -9
#define FS_ENAME    -10   /* ungueltiger Name */
#define FS_EROFS    -11   /* schreibgeschuetzt eingebunden */
#define FS_ENODRV   -12   /* Laufwerk nicht eingebunden */
#define FS_ENOSYS   -13

typedef struct {
    char name[256];
    uint64_t size;
    uint8_t is_dir;
} FsEntry;

typedef int (*FsDirCb)(const FsEntry *e, void *ctx);   /* Rueckgabe != 0 beendet das Auflisten */

typedef struct Fs {
    char label[16];
    char type[8];            /* "FAT32", "NTFS" */
    BlockDev *dev;
    void *priv;
    int readonly;            /* 1 = Schreiben gesperrt (Standard beim automatischen Einbinden) */

    int     (*stat)(struct Fs *fs, const char *path, FsEntry *out);
    int     (*readdir)(struct Fs *fs, const char *path, FsDirCb cb, void *ctx);
    int64_t (*read)(struct Fs *fs, const char *path, uint64_t off, void *buf, uint64_t len);
    int     (*write)(struct Fs *fs, const char *path, const void *buf, uint64_t len, int append);   /* legt an oder ersetzt */
    int     (*mkdir)(struct Fs *fs, const char *path);
    int     (*remove)(struct Fs *fs, const char *path);   /* Datei oder leeres Verzeichnis */
    int     (*rename)(struct Fs *fs, const char *from, const char *to);
    int     (*statfs)(struct Fs *fs, uint64_t *total_bytes, uint64_t *free_bytes);
} Fs;

const char *fs_strerror(int err);

#endif /* BIYOS_FS_H */
