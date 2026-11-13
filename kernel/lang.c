/* SPDX-License-Identifier: MIT */
/*
 * lang.c - Sprachen (Deutsch/Englisch) und Konfiguration in boot.cfg
 *
 * Die Quelltexte sind deutsch. Uebersetzt wird zentral bei der Ausgabe: con_puts() ruft tr() auf, das den uebergebenen Text
 * (ein Stueck, so wie er im Quelltext steht) in der Tabelle lang_en.inc sucht und bei Treffer die englische Fassung
 * ausgibt. Texte ohne Eintrag bleiben unveraendert. So brauchen Aufrufstellen (auch Module ueber kapi.puts) keine Aenderung.
 * tools/check-lang.py prueft die Tabelle gegen den Quelltext.
 */
#include "kernel.h"

int g_lang;   /* 0 = Deutsch, 1 = Englisch */

static const struct {
    const char *de, *en;
} g_tbl[] = {
#include "lang_en.inc"
};

static int same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

const char *tr(const char *s) {
    if (g_lang == 0 || !s) return s;
    for (size_t i = 0; i < sizeof(g_tbl) / sizeof(g_tbl[0]); i++)
        if (g_tbl[i].de[0] == s[0] && same(g_tbl[i].de, s)) return g_tbl[i].en;
    return s;
}

int lang_get(void) { return g_lang; }
void lang_set(int lang) { g_lang = lang ? 1 : 0; }

/* In boot.cfg (\\BIYOS\\boot.cfg auf dem ersten Laufwerk, das sie enthaelt) den Wert von key= setzen oder anhaengen.
 * where (mind. 4 Zeichen) bekommt "X:" des gefundenen Laufwerks. 0 = ok. */
int config_save(const char *key, const char *value, char *where) {
    size_t kl = 0, vl = 0;
    while (key[kl]) kl++;
    while (value[vl]) vl++;
    for (char d = 'A'; d <= 'Z'; d++) {
        Fs *fs = vfs_get(d);
        if (!fs) continue;
        char path[24] = {d, ':', '/', 'B', 'I', 'Y', 'O', 'S', '/', 'b', 'o', 'o', 't', '.', 'c', 'f', 'g', 0};
        FsEntry e;
        if (vfs_stat(path, &e) != 0 || e.is_dir) continue;
        uint8_t *buf;
        uint64_t len;
        if (vfs_read_all(path, &buf, &len) != 0) continue;
        uint8_t *out = kapi.malloc(len + kl + vl + 8);
        if (!out) {
            kapi.free(buf);
            return FS_ENOSPC;
        }
        size_t o = 0;
        int found = 0;
        uint64_t i = 0;
        while (i < len) {   /* zeilenweise kopieren, die key=-Zeile ersetzen */
            uint64_t ls = i;
            while (i < len && buf[i] != '\n') i++;
            uint64_t le = i;
            if (i < len) i++;
            uint64_t p = ls;
            while (p < le && (buf[p] == ' ' || buf[p] == '\t')) p++;
            int match = p + kl < le + 1 && buf[p] != '#';
            for (size_t k = 0; match && k < kl; k++)
                if (p + k >= le || buf[p + k] != (uint8_t)key[k]) match = 0;
            if (match && p + kl < le && buf[p + kl] == '=') {
                for (size_t k = 0; k < kl; k++) out[o++] = (uint8_t)key[k];
                out[o++] = '=';
                for (size_t k = 0; k < vl; k++) out[o++] = (uint8_t)value[k];
                out[o++] = '\n';
                found = 1;
            } else {
                for (uint64_t k = ls; k < le; k++) out[o++] = buf[k];
                if (i <= len && (le < len || (le == len && 0))) out[o++] = '\n';
            }
        }
        if (!found) {
            if (o && out[o - 1] != '\n') out[o++] = '\n';
            for (size_t k = 0; k < kl; k++) out[o++] = (uint8_t)key[k];
            out[o++] = '=';
            for (size_t k = 0; k < vl; k++) out[o++] = (uint8_t)value[k];
            out[o++] = '\n';
        }
        int was_ro = fs->readonly;
        int r = 0;
        if (was_ro) r = vfs_set_rw(d, 1);
        if (r == 0) r = vfs_write(path, out, o, 0);
        if (was_ro) vfs_set_rw(d, 0);
        kapi.free(out);
        kapi.free(buf);
        if (r == 0 && where) {
            where[0] = d;
            where[1] = ':';
            where[2] = 0;
        }
        return r;
    }
    return FS_ENOENT;
}
