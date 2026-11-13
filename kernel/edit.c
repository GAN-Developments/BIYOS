/* SPDX-License-Identifier: MIT */
/*
 * edit.c - Texteditor (nano-aehnlich) und Hex-Editor
 *
 *   edit <datei>        Text. Zeilenende LF oder CRLF (CRLF bleibt beim Speichern erhalten), Tab = 4 Spalten.
 *   edit -hex <datei>   Bytes. Hex-Ziffern ueberschreiben, Einfg fuegt ein 00-Byte ein, Entf loescht ein Byte.
 *   Strg+S speichern, Strg+X beenden (fragt bei Aenderungen nach). Existiert die Datei nicht, wird sie beim
 *   ersten Speichern angelegt. Laufwerke sind standardmaessig schreibgeschuetzt: vorher MOUNT <GERAET> <LW> RW.
 *
 * Der Editor zeichnet nur ueber con_put_at (Zellzugriff), braucht also den Framebuffer. Der alte Bildschirm
 * wird gesichert und beim Beenden zurueckgeholt. Ausgaben anderer Tasks gehen waehrenddessen nur an die
 * serielle Schnittstelle (con_hold), damit sie den Editor nicht uebermalen.
 *
 * Aufbau: ein einziger Puffer mit der ganzen Datei, Einfuegen und Loeschen verschieben Bytes (bis 8 MiB).
 */
#include "kernel.h"

#define ED_MAX   (8u * 1024 * 1024)
#define TABW     4
#define MAXW     256   /* breiter als jede Konsole (MAX_COLS = 240) */

#define C_FG     0xE6E6E6u
#define C_BG     0x080C18u
#define C_DIM    0x7C8AA8u
#define C_BAR_FG 0xFFFFFFu
#define C_BAR_BG 0x3A5FC8u
#define C_CUR_FG 0x080C18u
#define C_CUR_BG 0xE6E6E6u
#define C_MSG    0xFFDC78u

typedef struct {
    uint8_t *buf;
    size_t   len, cap;
    char     path[256];
    int      hex, modified, crlf;
    size_t   cur;        /* Text: Byte-Offset des Cursors; Hex: Byte-Index */
    size_t   top;        /* Text: Offset der ersten sichtbaren Zeile; Hex: erste sichtbare Zeile */
    int      left;       /* Text: erste sichtbare Spalte */
    int      want_col;   /* Text: Wunschspalte fuer hoch/runter */
    int      nib;        /* Hex: 0 = hohes, 1 = niedriges Halbbyte */
    int      w, h;       /* nutzbare Flaeche in Zeichen */
    char     msg[96];
} Ed;

/* ------------------------------------------------------------------ */
/* Kleine Helfer (ohne libc)                                           */
/* ------------------------------------------------------------------ */
static size_t slen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void set_msg(Ed *e, const char *a, const char *b) {
    size_t n = 0;
    a = tr(a);
    b = tr(b);
    for (; a && *a && n < sizeof(e->msg) - 1; a++) e->msg[n++] = *a;
    for (; b && *b && n < sizeof(e->msg) - 1; b++) e->msg[n++] = *b;
    e->msg[n] = 0;
}

/* schreibt s ab Zelle *pos in cells (und schiebt *pos), bis max erreicht ist */
static void cat(char *cells, int *pos, int max, const char *s) {
    while (*s && *pos < max) cells[(*pos)++] = *s++;
}

static void cat_dec(char *cells, int *pos, int max, uint64_t v) {
    char t[24];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 23);
    while (n && *pos < max) cells[(*pos)++] = t[--n];
}

static void cat_hex(char *cells, int *pos, int max, uint64_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0 && *pos < max; i--) cells[(*pos)++] = hx[(v >> (4 * i)) & 0xF];
}

static const char *base_of(const char *p) {
    const char *b = p;
    for (; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    return b;
}

static int getkey_wait(void) {
    int k;
    while ((k = kbd_getc()) < 0) kapi.yield();
    return k;
}

/* ------------------------------------------------------------------ */
/* Puffer                                                              */
/* ------------------------------------------------------------------ */
static int ed_insert(Ed *e, size_t off, const uint8_t *d, size_t n) {
    if (e->len + n > ED_MAX) return -1;
    if (e->len + n + 1 > e->cap) {
        size_t nc = e->cap ? e->cap : 256;
        while (nc < e->len + n + 1) nc *= 2;
        uint8_t *nb = kapi.realloc(e->buf, nc);
        if (!nb) return -1;
        e->buf = nb;
        e->cap = nc;
    }
    for (size_t i = e->len; i > off; i--) e->buf[i - 1 + n] = e->buf[i - 1];
    for (size_t i = 0; i < n; i++) e->buf[off + i] = d[i];
    e->len += n;
    e->modified = 1;
    return 0;
}

static void ed_delete(Ed *e, size_t off, size_t n) {
    for (size_t i = off; i + n < e->len; i++) e->buf[i] = e->buf[i + n];
    e->len -= n;
    e->modified = 1;
}

/* ------------------------------------------------------------------ */
/* Text: Zeilen und Spalten                                            */
/* ------------------------------------------------------------------ */
static int adv(int col, uint8_t c) {
    return c == '\t' ? (col / TABW + 1) * TABW : col + 1;
}

static size_t ls_of(const Ed *e, size_t off) {   /* Anfang der Zeile, die off enthaelt */
    while (off > 0 && e->buf[off - 1] != '\n') off--;
    return off;
}

static size_t le_of(const Ed *e, size_t off) {   /* Ende der Zeile (Position des '\n' oder len) */
    while (off < e->len && e->buf[off] != '\n') off++;
    return off;
}

static int col_of(const Ed *e, size_t ls, size_t off) {
    int c = 0;
    for (size_t i = ls; i < off; i++) c = adv(c, e->buf[i]);
    return c;
}

static size_t off_at_col(const Ed *e, size_t ls, int want) {   /* Offset in der Zeile, dessen Spalte <= want ist */
    size_t i = ls;
    int c = 0;
    while (i < e->len && e->buf[i] != '\n') {
        int n = adv(c, e->buf[i]);
        if (n > want) break;
        c = n;
        i++;
    }
    return i;
}

static void move_up(Ed *e) {
    size_t ls = ls_of(e, e->cur);
    if (ls == 0) return;
    e->cur = off_at_col(e, ls_of(e, ls - 1), e->want_col);
}

static void move_down(Ed *e) {
    size_t le = le_of(e, e->cur);
    if (le >= e->len) return;
    e->cur = off_at_col(e, le + 1, e->want_col);
}

static void sync_col(Ed *e) {
    e->want_col = col_of(e, ls_of(e, e->cur), e->cur);
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */
static void flush_row(Ed *e, int y, const char *cell, const uint8_t *attr) {
    for (int x = 0; x < e->w; x++) {
        uint8_t a = attr[x];
        uint32_t fg = a == 1 ? C_CUR_FG : a == 2 ? C_BAR_FG : a == 3 ? C_MSG : a == 4 ? C_DIM : C_FG;
        uint32_t bg = a == 1 ? C_CUR_BG : a == 2 ? C_BAR_BG : C_BG;
        con_put_at(x, y, cell[x], fg, bg);
    }
}

static void bar_row(Ed *e, int y, const char *left, const char *right) {
    char cell[MAXW];
    uint8_t attr[MAXW];
    for (int x = 0; x < e->w; x++) {
        cell[x] = ' ';
        attr[x] = 2;
    }
    int p = 0;
    cat(cell, &p, e->w, left);
    right = tr(right);
    int rl = (int)slen(right);
    if (p + 2 + rl <= e->w) {
        int q = e->w - rl;
        cat(cell, &q, e->w, right);
    }
    flush_row(e, y, cell, attr);
}

static void msg_row(Ed *e) {
    char cell[MAXW];
    uint8_t attr[MAXW];
    for (int x = 0; x < e->w; x++) {
        cell[x] = ' ';
        attr[x] = 3;
    }
    int p = 0;
    cat(cell, &p, e->w, e->msg);
    flush_row(e, e->h - 1, cell, attr);
}

static void render_text_line(Ed *e, int y, size_t ls, int has, int cursor_x) {
    char cell[MAXW];
    uint8_t attr[MAXW];
    for (int x = 0; x < e->w; x++) {
        cell[x] = ' ';
        attr[x] = 0;
    }
    if (has) {
        size_t i = ls;
        int col = 0;
        while (i < e->len && e->buf[i] != '\n') {
            uint8_t c = e->buf[i++];
            if (c == '\t') {
                col = adv(col, c);
            } else {
                int sx = col - e->left;
                if (sx >= 0 && sx < e->w) cell[sx] = (c >= 32 && c < 127) ? (char)c : '?';
                col++;
            }
            if (col - e->left >= e->w) break;
        }
    }
    if (cursor_x >= 0 && cursor_x < e->w) attr[cursor_x] = 1;
    flush_row(e, y, cell, attr);
}

static void draw_text(Ed *e) {
    int th = e->h - 2;
    if (e->top > e->len) e->top = e->len;
    e->top = ls_of(e, e->top);

    size_t cls = ls_of(e, e->cur);
    int ccol = col_of(e, cls, e->cur);
    if (ccol < e->left) e->left = ccol;
    else if (ccol >= e->left + e->w) e->left = ccol - e->w + 1;

    if (cls < e->top) {
        e->top = cls;
    } else {
        int n = 0;
        for (size_t p = e->top; p < cls; p = le_of(e, p) + 1) n++;
        while (n >= th) {
            e->top = le_of(e, e->top) + 1;
            n--;
        }
    }

    size_t p = e->top;
    int has = 1;
    for (int y = 0; y < th; y++) {
        size_t next = has ? le_of(e, p) : 0;
        render_text_line(e, y, p, has, (has && p == cls) ? ccol - e->left : -1);
        if (has) {
            if (next >= e->len) has = 0;
            else p = next + 1;
        }
    }

    size_t line = 1;
    for (size_t i = 0; i < e->cur; i++)
        if (e->buf[i] == '\n') line++;
    char st[MAXW];
    int sp = 0;
    cat(st, &sp, MAXW - 1, " ");
    cat(st, &sp, MAXW - 1, base_of(e->path));
    if (e->modified) cat(st, &sp, MAXW - 1, " [*]");
    cat(st, &sp, MAXW - 1, g_lang ? "   L " : "   Z ");   /* Zeile / line */
    cat_dec(st, &sp, MAXW - 1, line);
    cat(st, &sp, MAXW - 1, g_lang ? " C " : " S ");         /* Spalte / column */
    cat_dec(st, &sp, MAXW - 1, (uint64_t)ccol + 1);
    st[sp] = 0;
    bar_row(e, e->h - 2, st, "^S SPEICHERN  ^X ENDE ");
    msg_row(e);
}

static int hex_bpr(const Ed *e) {
    return e->w >= 76 ? 16 : 8;
}

static void draw_hex(Ed *e) {
    int th = e->h - 2, bpr = hex_bpr(e);
    size_t crow = e->cur / (size_t)bpr;
    if (crow < e->top) e->top = crow;
    else if (crow >= e->top + (size_t)th) e->top = crow - (size_t)th + 1;
    int hx0 = 10, asc0 = hx0 + bpr * 3 + 1;

    for (int y = 0; y < th; y++) {
        char cell[MAXW];
        uint8_t attr[MAXW];
        for (int x = 0; x < e->w; x++) {
            cell[x] = ' ';
            attr[x] = 0;
        }
        size_t row = e->top + (size_t)y;
        size_t base = row * (size_t)bpr;
        if (base <= e->len) {
            int p = 0;
            cat_hex(cell, &p, e->w, base, 8);
            for (int x = 0; x < 8; x++) attr[x] = 4;
            for (int b = 0; b < bpr; b++) {
                size_t idx = base + (size_t)b;
                int hp = hx0 + b * 3;
                if (idx < e->len) {
                    int q = hp;
                    cat_hex(cell, &q, e->w, e->buf[idx], 2);
                    uint8_t c = e->buf[idx];
                    cell[asc0 + b] = (c >= 32 && c < 127) ? (char)c : '.';
                    if (c < 32 || c >= 127) attr[asc0 + b] = 4;
                }
                if (idx == e->cur) {   /* Cursor: das aktive Halbbyte und das ASCII-Zeichen */
                    attr[hp + e->nib] = 1;
                    attr[asc0 + b] = 1;
                }
            }
        }
        flush_row(e, y, cell, attr);
    }
    char st[MAXW];
    int sp = 0;
    cat(st, &sp, MAXW - 1, " HEX ");
    cat(st, &sp, MAXW - 1, base_of(e->path));
    if (e->modified) cat(st, &sp, MAXW - 1, " [*]");
    cat(st, &sp, MAXW - 1, "   OFFSET ");
    cat_hex(st, &sp, MAXW - 1, e->cur, 8);
    cat(st, &sp, MAXW - 1, " / ");
    cat_dec(st, &sp, MAXW - 1, e->len);
    st[sp] = 0;
    bar_row(e, e->h - 2, st, "^S SPEICHERN  ^X ENDE  EINFG/ENTF ");
    msg_row(e);
}

/* ------------------------------------------------------------------ */
/* Laden und Speichern                                                 */
/* ------------------------------------------------------------------ */
static int ed_load(Ed *e) {
    uint8_t *buf = 0;
    uint64_t len = 0;
    int r = vfs_read_all(e->path, &buf, &len);
    if (r == FS_ENOENT) {   /* neue Datei */
        buf = kapi.malloc(256);
        if (!buf) return FS_ENOSPC;
        buf[0] = 0;
        e->buf = buf;
        e->cap = 256;
        e->len = 0;
        set_msg(e, "NEUE DATEI", 0);
        return 0;
    }
    if (r) return r;
    if (len > ED_MAX) {
        kapi.free(buf);
        return FS_ENOSPC;
    }
    e->buf = buf;
    e->len = (size_t)len;
    e->cap = (size_t)len + 1;

    if (!e->hex) {
        /* CRLF merken und beim Laden zu LF machen, beim Speichern wieder herstellen */
        size_t o = 0;
        for (size_t i = 0; i < e->len; i++) {
            if (e->buf[i] == '\r' && i + 1 < e->len && e->buf[i + 1] == '\n') {
                e->crlf = 1;
                continue;
            }
            e->buf[o++] = e->buf[i];
        }
        e->len = o;
        e->buf[o] = 0;
        for (size_t i = 0; i < e->len; i++)
            if (e->buf[i] == 0) {
                set_msg(e, "ENTHAELT NUL-BYTES: BINAERDATEI? BESSER EDIT -HEX", 0);
                break;
            }
    }
    return 0;
}

static int ed_save(Ed *e) {
    uint8_t *out = e->buf, *tmp = 0;
    size_t olen = e->len;
    if (e->crlf) {
        size_t nl = 0;
        for (size_t i = 0; i < e->len; i++)
            if (e->buf[i] == '\n') nl++;
        if (nl) {
            tmp = kapi.malloc(e->len + nl + 1);
            if (!tmp) {
                set_msg(e, "KEIN SPEICHER ZUM SPEICHERN", 0);
                return -1;
            }
            size_t o = 0;
            for (size_t i = 0; i < e->len; i++) {
                if (e->buf[i] == '\n') tmp[o++] = '\r';
                tmp[o++] = e->buf[i];
            }
            out = tmp;
            olen = o;
        }
    }
    int r = vfs_write(e->path, out, olen, 0);
    if (tmp) kapi.free(tmp);
    if (r) {
        set_msg(e, "FEHLER: ", exec_strerror(r));
        return r;
    }
    e->modified = 0;
    char num[24];
    int p = 0;
    cat_dec(num, &p, 23, olen);
    num[p] = 0;
    set_msg(e, "GESPEICHERT: ", num);
    return 0;
}

/* Fragt "speichern?" und liefert 1 = beenden, 0 = weiter editieren */
static int ask_quit(Ed *e) {
    set_msg(e, "AENDERUNGEN SPEICHERN? J = JA  N = NEIN  ESC = ZURUECK", 0);
    msg_row(e);
    int k = getkey_wait();
    if (k == 'j' || k == 'J' || k == 'y' || k == 'Y') {
        if (ed_save(e) == 0) return 1;
        return 0;   /* Speichern ist fehlgeschlagen: Meldung zeigen und weitermachen */
    }
    if (k == 'n' || k == 'N') return 1;
    e->msg[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tasten                                                              */
/* ------------------------------------------------------------------ */
static int hexval(int k) {
    if (k >= '0' && k <= '9') return k - '0';
    if (k >= 'a' && k <= 'f') return k - 'a' + 10;
    if (k >= 'A' && k <= 'F') return k - 'A' + 10;
    return -1;
}

static void key_text(Ed *e, int k) {
    int th = e->h - 2;
    if (k == KEY_LEFT) {
        if (e->cur > 0) e->cur--;
        sync_col(e);
    } else if (k == KEY_RIGHT) {
        if (e->cur < e->len) e->cur++;
        sync_col(e);
    } else if (k == KEY_UP) {
        move_up(e);
    } else if (k == KEY_DOWN) {
        move_down(e);
    } else if (k == KEY_HOME || k == 1) {   /* Pos1 oder Strg+A */
        e->cur = ls_of(e, e->cur);
        sync_col(e);
    } else if (k == KEY_END || k == 5) {    /* Ende oder Strg+E */
        e->cur = le_of(e, e->cur);
        sync_col(e);
    } else if (k == KEY_PGUP) {
        for (int i = 0; i < th - 1; i++) move_up(e);
    } else if (k == KEY_PGDN) {
        for (int i = 0; i < th - 1; i++) move_down(e);
    } else if (k == KEY_DEL) {
        if (e->cur < e->len) ed_delete(e, e->cur, 1);
    } else if (k == '\b') {
        if (e->cur > 0) {
            ed_delete(e, e->cur - 1, 1);
            e->cur--;
            sync_col(e);
        }
    } else if (k == '\n' || k == '\t' || (k >= 32 && k < 127)) {
        uint8_t c = (uint8_t)k;
        if (ed_insert(e, e->cur, &c, 1) != 0) {
            set_msg(e, "KEIN SPEICHER / DATEI ZU GROSS", 0);
            return;
        }
        e->cur++;
        sync_col(e);
    }
}

static void key_hex(Ed *e, int k) {
    int bpr = hex_bpr(e), th = e->h - 2;
    size_t b = (size_t)bpr;
    if (k == KEY_LEFT || k == '\b') {
        if (e->cur > 0) e->cur--;
        e->nib = 0;
    } else if (k == KEY_RIGHT) {
        if (e->cur < e->len) e->cur++;
        e->nib = 0;
    } else if (k == KEY_UP) {
        if (e->cur >= b) e->cur -= b;
        e->nib = 0;
    } else if (k == KEY_DOWN) {
        if (e->cur + b <= e->len) e->cur += b;
        e->nib = 0;
    } else if (k == KEY_HOME) {
        e->cur -= e->cur % b;
        e->nib = 0;
    } else if (k == KEY_END) {
        size_t end = e->cur - e->cur % b + b - 1;
        e->cur = end < e->len ? end : e->len;
        e->nib = 0;
    } else if (k == KEY_PGUP) {
        size_t d = (size_t)(th - 1) * b;
        e->cur = e->cur >= d ? e->cur - d : e->cur % b;
        e->nib = 0;
    } else if (k == KEY_PGDN) {
        size_t d = (size_t)(th - 1) * b;
        e->cur = e->cur + d <= e->len ? e->cur + d : e->len;
        e->nib = 0;
    } else if (k == KEY_INS) {
        uint8_t z = 0;
        if (ed_insert(e, e->cur, &z, 1) != 0) set_msg(e, "KEIN SPEICHER / DATEI ZU GROSS", 0);
        e->nib = 0;
    } else if (k == KEY_DEL) {
        if (e->cur < e->len) ed_delete(e, e->cur, 1);
        e->nib = 0;
    } else if (hexval(k) >= 0) {
        int v = hexval(k);
        if (e->cur == e->len) {   /* hinter dem Ende: neues Byte anhaengen */
            uint8_t z = 0;
            if (ed_insert(e, e->len, &z, 1) != 0) {
                set_msg(e, "KEIN SPEICHER / DATEI ZU GROSS", 0);
                return;
            }
        }
        if (e->nib == 0) {
            e->buf[e->cur] = (uint8_t)((e->buf[e->cur] & 0x0F) | (v << 4));
            e->nib = 1;
        } else {
            e->buf[e->cur] = (uint8_t)((e->buf[e->cur] & 0xF0) | v);
            e->nib = 0;
            e->cur++;
        }
        e->modified = 1;
    }
}

/* ------------------------------------------------------------------ */
int edit_run(const char *path, int hex) {
    int w, h;
    if (con_size(&w, &h) != 0 || w < (hex ? 44 : 40) || h < 8 || w > MAXW) return FS_ENOSYS;
    Ed *e = kapi.malloc(sizeof(Ed));
    if (!e) return FS_ENOSPC;
    char *z = (char *)e;
    for (size_t i = 0; i < sizeof(Ed); i++) z[i] = 0;
    size_t n = 0;
    for (; path[n] && n < sizeof(e->path) - 1; n++) e->path[n] = path[n];
    e->path[n] = 0;
    e->hex = hex;
    e->w = w;
    e->h = h;

    int r = ed_load(e);
    if (r) {
        kapi.free(e);
        return r;
    }

    int saved = con_save() == 0;
    con_hold(1);
    for (int y = 0; y < h; y++)   /* alten Bildschirm leeren */
        for (int x = 0; x < w; x++) con_put_at(x, y, ' ', C_FG, C_BG);

    for (;;) {
        if (hex) draw_hex(e);
        else draw_text(e);
        int k = getkey_wait();
        e->msg[0] = 0;
        if (k == 24) {   /* Strg+X */
            if (!e->modified || ask_quit(e)) break;
        } else if (k == 19) {   /* Strg+S */
            ed_save(e);
        } else if (hex) {
            key_hex(e, k);
        } else {
            key_text(e, k);
        }
    }

    con_hold(0);
    if (saved) con_restore();
    else con_reset();
    kapi.free(e->buf);
    kapi.free(e);
    return 0;
}
