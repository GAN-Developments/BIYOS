/* STRT in C: eine kleine Ersatz-Shell. Legt man sie nach C:\Start, ersetzt sie beim Boot die eingebaute Shell.
 * Befehle: HELP DIR [PFAD] CD <PFAD> X: TYPE <DATEI> ECHO .. CLEAR VER EXIT, alles andere wird als Programm
 * ausgefuehrt (PLSE/STRT). EXIT startet die eingebaute Shell wieder. Bauen: siehe build.sh */
#include "kapi.h"

static KernelApi *A;

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int streq_ci(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return *a == *b;
}

static void error(int r) {
    A->color(255, 120, 120);
    A->puts("FEHLER: ");
    A->puts(A->strerror(r));
    A->putc('\n');
}

/* Zeile lesen, mit Backspace und Ctrl+C (wie die eingebaute Shell) */
static void readline(char *line, int max) {
    int n = 0;
    for (;;) {
        A->putc('_');   /* Cursor */
        int c;
        while ((c = A->getkey()) < 0) A->yield();
        A->con_backspace();
        if (c == '\n') { A->putc('\n'); line[n] = 0; return; }
        if (c == 3)    { A->puts("^C\n"); line[0] = 0; return; }
        if (c == '\b') { if (n > 0) { n--; A->con_backspace(); } continue; }
        if (c >= 32 && c < 127 && n < max - 1) { line[n++] = (char)c; A->putc((char)c); }
    }
}

static int split(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    return argc;
}

static int dir_cb(const FsEntry *e, void *ctx) {
    (void)ctx;
    A->puts("  ");
    if (e->is_dir) A->puts("<DIR>  ");
    else { A->dec(e->size); A->puts("  "); }
    A->puts(e->name);
    A->putc('\n');
    return 0;
}

static void prompt(void) {
    A->color(255, 200, 120);
    A->puts("MSH");
    if (A->cur_drive()) {
        A->putc(' ');
        A->putc(A->cur_drive());
        A->putc(':');
        A->puts(A->cur_path());
    }
    A->puts("> ");
    A->color(230, 230, 230);
}

static void help(void) {
    A->puts("HELP  DIR [PFAD]  CD <PFAD>  X:  TYPE <DATEI>  ECHO ..  CLEAR  VER  EXIT\n");
    A->puts("SONST: PROGRAMM (PLSE/STRT) AUSFUEHREN. EXIT STARTET DIE EINGEBAUTE SHELL WIEDER.\n");
}

__attribute__((section(".text.entry"))) int entry(KernelApi *api, int argc, char **argv) {
    (void)argc; (void)argv;
    A = api;
    if (api->version < 7) return 1;   /* braucht shell_stop & Co. */
    api->shell_stop();                /* eingebaute Shell weg, ab jetzt gehoert die Eingabe uns */
    api->color(255, 200, 120);
    api->puts("\nMSH - ERSATZ-SHELL (HELP, EXIT STARTET DIE EINGEBAUTE)\n");

    char line[200];
    char *av[16];
    for (;;) {
        prompt();
        readline(line, sizeof(line));
        int n = split(line, av, 16);
        if (!n) continue;
        const char *c = av[0];
        int r = 0;
        if (streq_ci(c, "exit")) break;
        else if (streq_ci(c, "help") || streq(c, "?")) help();
        else if (streq_ci(c, "clear") || streq_ci(c, "cls")) api->con_clear();
        else if (streq_ci(c, "ver")) { api->puts("MSH, KAPI V"); api->dec(api->version); api->putc('\n'); }
        else if (streq_ci(c, "echo")) {
            for (int i = 1; i < n; i++) { if (i > 1) api->putc(' '); api->puts(av[i]); }
            api->putc('\n');
        }
        else if (streq_ci(c, "dir")) {
            r = api->file_list(n > 1 ? av[1] : ".", dir_cb, 0);
            if (r) error(r);
        }
        else if (streq_ci(c, "cd")) {
            if (n < 2) { api->puts(api->cur_path()); api->putc('\n'); }
            else if ((r = api->chdir(av[1])) != 0) error(r);
        }
        else if (c[0] && c[1] == ':' && !c[2]) {
            if ((r = api->set_drive(c[0] >= 'a' ? (char)(c[0] - 32) : c[0])) != 0) error(r);
        }
        else if (streq_ci(c, "type")) {
            if (n < 2) { api->puts("TYPE <DATEI>\n"); continue; }
            uint8_t *buf; uint64_t len;
            if ((r = api->file_read_all(av[1], &buf, &len)) != 0) { error(r); continue; }
            api->puts((const char *)buf);
            api->free(buf);
        }
        else {
            int ret = 0;
            r = api->exec_run(c, n, av, &ret);
            if (r == FS_ENOENT) { api->puts("UNBEKANNTER BEFEHL: "); api->puts(c); api->putc('\n'); }
            else if (r) error(r);
            else if (ret) { api->puts("RUECKGABEWERT "); api->dec((uint64_t)(int64_t)ret); api->putc('\n'); }
        }
    }
    api->shell_start();   /* zurueck zur eingebauten Shell */
    return 0;
}
