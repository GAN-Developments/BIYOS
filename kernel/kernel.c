/*
 * BIYOS Test-Kernel
 *
 * Wechselt auf einen eigenen Stack, initialisiert COM1 und den Framebuffer, gibt die
 * BootInfo aus, richtet GDT/TSS, IDT und flache Seitentabellen ein (arch.c) und fuehrt
 * einen kleinen Selbsttest der Interrupt-Behandlung durch.
 *
 * Mit  make EXTRA=-DBIYOS_TEST_FAULT=1|2|3  loest der Kernel absichtlich eine Exception
 * aus (1 = #UD, 2 = #DE, 3 = #PF, 4 = #PF nach unmap), um die Fehlerausgabe zu testen.
 */
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* Einstieg: eigener Stack, rdi (BootInfo*) bleibt unveraendert        */
/* ------------------------------------------------------------------ */
__asm__(
    ".section .bss\n"
    ".align 16\n"
    "stack_bottom:\n"
    ".skip 32768\n"
    "stack_top:\n"
    ".section .text\n"
    ".global _start\n"
    "_start:\n"
    "    cli\n"
    "    lea stack_top(%rip), %rsp\n"
    "    xor %ebp, %ebp\n"
    "    call kmain\n"
    "1:  hlt\n"
    "    jmp 1b\n");

/* ------------------------------------------------------------------ */
/* Minimal-libc (GCC darf Aufrufe darauf erzeugen)                     */
/* ------------------------------------------------------------------ */
#define NOPAT __attribute__((optimize("no-tree-loop-distribute-patterns")))

NOPAT void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

NOPAT void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}

/* ------------------------------------------------------------------ */
/* Serielle Schnittstelle COM1 (38400 Baud, 8N1)                       */
/* ------------------------------------------------------------------ */
static void serial_init(void) {
    outb(0x3F9, 0x00);   /* Interrupts aus */
    outb(0x3FB, 0x80);   /* DLAB an */
    outb(0x3F8, 0x03);   /* Teiler 3 = 38400 Baud */
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03);   /* 8 Bit, keine Paritaet, 1 Stoppbit */
    outb(0x3FA, 0xC7);   /* FIFO an */
    outb(0x3FC, 0x0B);
}

static void serial_putc(char c) {
    for (int i = 0; i < 100000 && !(inb(0x3FD) & 0x20); i++) {
    }
    outb(0x3F8, (uint8_t)c);
}

/* ------------------------------------------------------------------ */
/* Framebuffer und 5x7-Schrift (A-Z, 0-9 und einige Zeichen)           */
/* ------------------------------------------------------------------ */
static uint8_t *fb;
static uint32_t fb_w, fb_h, fb_pitch, fb_fmt;
static int scale = 2;
static uint32_t fg;

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (fb_fmt == FB_FORMAT_RGBX) return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;   /* BGRX */
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb) return;
    for (uint32_t j = 0; j < h && y + j < fb_h; j++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)(y + j) * fb_pitch);
        for (uint32_t i = 0; i < w && x + i < fb_w; i++) row[x + i] = color;
    }
}

static const uint8_t font_alpha[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
};

static const uint8_t font_digit[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
};

static const uint8_t g_space[7]  = {0, 0, 0, 0, 0, 0, 0};
static const uint8_t g_colon[7]  = {0, 0x04, 0, 0, 0, 0x04, 0};
static const uint8_t g_dash[7]   = {0, 0, 0, 0x1F, 0, 0, 0};
static const uint8_t g_dot[7]    = {0, 0, 0, 0, 0, 0x0C, 0x0C};
static const uint8_t g_slash[7]  = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
static const uint8_t g_comma[7]  = {0, 0, 0, 0, 0x0C, 0x04, 0x08};
static const uint8_t g_equal[7]  = {0, 0, 0x1F, 0, 0x1F, 0, 0};
static const uint8_t g_under[7]  = {0, 0, 0, 0, 0, 0, 0x1F};
static const uint8_t g_plus[7]   = {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0};
static const uint8_t g_hash[7]   = {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A};
static const uint8_t g_lpar[7]   = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
static const uint8_t g_rpar[7]   = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
static const uint8_t g_unk[7]    = {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F};

static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return font_alpha[c - 'A'];
    if (c >= '0' && c <= '9') return font_digit[c - '0'];
    switch (c) {
    case ' ': return g_space;
    case ':': return g_colon;
    case '-': return g_dash;
    case '.': return g_dot;
    case '/': return g_slash;
    case ',': return g_comma;
    case '=': return g_equal;
    case '_': return g_under;
    case '+': return g_plus;
    case '#': return g_hash;
    case '(': return g_lpar;
    case ')': return g_rpar;
    default:  return g_unk;
    }
}

/* ------------------------------------------------------------------ */
/* Textkonsole (Framebuffer + serielle Spiegelung)                     */
/* ------------------------------------------------------------------ */
static uint32_t cx, cy, cols, rows;

#define CELL_W (6 * (uint32_t)scale)
#define CELL_H (8 * (uint32_t)scale)

static void con_clear(void) {
    fill_rect(0, 0, fb_w, fb_h, rgb(8, 12, 24));
    cx = 1;
    cy = 1;
}

void con_color(uint8_t r, uint8_t g, uint8_t b) {
    fg = rgb(r, g, b);
}

static void con_putc_raw(char c) {
    if (c == '\n') {
        serial_putc('\r');
        serial_putc('\n');
    } else {
        serial_putc(c);
    }
    if (!fb) return;

    if (c == '\n') {
        cx = 1;
        cy++;
    } else {
        if (cx >= cols - 1) {
            cx = 1;
            cy++;
        }
        if (cy >= rows - 1) con_clear();
        const uint8_t *g = glyph(c);
        uint32_t x = cx * CELL_W, y = cy * CELL_H;
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] & (1 << (4 - b)))
                    fill_rect(x + (uint32_t)b * (uint32_t)scale, y + (uint32_t)r * (uint32_t)scale,
                              (uint32_t)scale, (uint32_t)scale, fg);
        cx++;
    }
    if (cy >= rows - 1) con_clear();
}

void con_putc(char c) {
    uint64_t f = irq_save();
    con_putc_raw(c);
    irq_restore(f);
}

void con_puts(const char *s) {
    uint64_t f = irq_save();   /* Zeichenkette am Stueck ausgeben, auch bei mehreren Tasks */
    while (*s) con_putc_raw(*s++);
    irq_restore(f);
}

void con_hex(uint64_t v) {
    static const char h[] = "0123456789ABCDEF";
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        if (d || started || i == 0) {
            con_putc(h[d]);
            started = 1;
        }
    }
}

void con_hex64(uint64_t v) {
    static const char h[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) con_putc(h[(v >> i) & 0xF]);
}

void con_dec(uint64_t v) {
    char t[21];
    int i = 20;
    t[i] = 0;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    con_puts(&t[i]);
}

void kernel_halt(void) {
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ */
/* Kernel-Funktionstabelle: alle Dienste hier eintragen, damit sie       */
/* von Programmen genutzt und ersetzt werden koennen (siehe kapi.h)      */
/* ------------------------------------------------------------------ */
KernelApi kapi = {
    .size = sizeof(KernelApi),
    .version = KAPI_VERSION,
    .putc = con_putc,
    .puts = con_puts,
    .hex = con_hex,
    .dec = con_dec,
    .color = con_color,
    .page_alloc = pmm_alloc_pages,
    .page_free = pmm_free_pages,
    .malloc = heap_malloc,
    .free = heap_free,
    .realloc = heap_realloc,
    .map = paging_map,
    .unmap = paging_unmap,
    .root = paging_root,
    .set_handler = idt_set_handler,
    .task_create = task_create,
    .task_kill = task_kill,
    .task_exit = task_exit,
    .yield = sched_yield,
    .sleep_ms = sched_sleep_ms,
    .uptime_ms = timer_uptime_ms,
};

/* ------------------------------------------------------------------ */
/* Selbsttest der Interrupt-Behandlung                                 */
/* ------------------------------------------------------------------ */
static volatile int test_counter;

static void test_handler(Regs *r) {
    (void)r;
    test_counter++;
}

/* ------------------------------------------------------------------ */
/* Selbsttest der Speicherverwaltung (alles ueber die Tabelle kapi)    */
/* ------------------------------------------------------------------ */
static int g_malloc_calls;
static void *(*g_orig_malloc)(size_t);

static void *counting_malloc(size_t n) {
    g_malloc_calls++;
    return g_orig_malloc(n);
}

static void report(const char *name, int ok) {
    con_puts(name);
    con_puts(ok ? ": OK\n" : ": FEHLGESCHLAGEN\n");
}

static void mm_selftest(void) {
    /* 1. physische Seiten */
    uint64_t free0 = pmm_free_page_count();
    uint64_t a = kapi.page_alloc(4), b = kapi.page_alloc(1);
    int ok = a && b && a != b;
    if (ok) {
        uint64_t *pa = (uint64_t *)(uintptr_t)a;
        for (int i = 0; i < 2048; i++) pa[i] = 0x1111111100000000ULL + (uint64_t)i;
        for (int i = 0; i < 2048; i++)
            if (pa[i] != 0x1111111100000000ULL + (uint64_t)i) ok = 0;
    }
    kapi.page_free(a, 4);
    kapi.page_free(b, 1);
    report("PMM TEST", ok && pmm_free_page_count() == free0);

    /* 2. Heap: Muster, realloc, Zusammenfuehren */
    uint8_t *p1 = kapi.malloc(100), *p2 = kapi.malloc(5000), *p3 = kapi.malloc(40);
    ok = p1 && p2 && p3;
    if (ok) {
        memset(p1, 0xAA, 100);
        memset(p2, 0xBB, 5000);
        memset(p3, 0xCC, 40);
        kapi.free(p2);
        p1 = kapi.realloc(p1, 3000);
        ok = p1 != 0;
        for (int i = 0; ok && i < 100; i++)
            if (p1[i] != 0xAA) ok = 0;
        for (int i = 0; ok && i < 40; i++)
            if (p3[i] != 0xCC) ok = 0;
        kapi.free(p1);
        kapi.free(p3);
    }
    void *big = kapi.malloc(100 * 1024);   /* nach dem Freigeben muss das ohne neues Segment passen */
    ok = ok && big;
    kapi.free(big);
    report("HEAP TEST", ok);

    /* 3. Paging: neue Zuordnung, Aufteilen einer 2-MiB-Seite, Aufheben */
    uint64_t phys = kapi.page_alloc(1), virt = 0x8000000000ULL;   /* 512 GiB: eigener PML4-Eintrag */
    ok = phys && kapi.map(virt, phys, 4096, PG_WRITE) == 0;
    if (ok) {
        *(volatile uint64_t *)virt = 0xB1405DEADBEEFULL;
        ok = *(volatile uint64_t *)(uintptr_t)phys == 0xB1405DEADBEEFULL;
    }
    uint64_t q = kapi.page_alloc(1);
    ok = ok && q && kapi.map(q, q, 4096, PG_WRITE) == 0;   /* teilt die 2-MiB-Seite von q auf */
    if (ok) {
        *(volatile uint64_t *)(uintptr_t)q = 0x1234;
        ok = *(volatile uint64_t *)(uintptr_t)q == 0x1234;
    }
    ok = ok && kapi.unmap(virt, 4096) == 0;
    kapi.page_free(phys, 1);
    kapi.page_free(q, 1);
    report("PAGING TEST", ok);

    /* 4. Tabelle ueberschreiben: ein Programm ersetzt kapi.malloc durch einen Zaehler */
    g_orig_malloc = kapi.malloc;
    g_malloc_calls = 0;
    kapi.malloc = counting_malloc;
    void *x = kapi.malloc(32);
    kapi.free(x);
    kapi.malloc = g_orig_malloc;
    report("API OVERRIDE TEST", x && g_malloc_calls == 1);
}

/* ------------------------------------------------------------------ */
/* Selbsttest von Timer und Scheduler                                  */
/* ------------------------------------------------------------------ */
static volatile uint64_t g_spin;
static volatile int g_done;

/* Rechnet nur und gibt die CPU nie ab: beweist, dass der Timer praeemptiv umschaltet */
static void spinner(void *arg) {
    (void)arg;
    for (;;) g_spin++;
}

static void worker(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    for (int i = 0; i < 3; i++) {
        con_puts("  TASK ");
        con_dec(id);
        con_puts(": SCHRITT ");
        con_dec((uint64_t)i);
        con_puts(" BEI ");
        con_dec(kapi.uptime_ms());
        con_puts(" MS\n");
        kapi.sleep_ms(50 * id);
    }
    __atomic_fetch_add(&g_done, 1, __ATOMIC_SEQ_CST);
}

static void sched_selftest(void) {
    uint64_t t0 = kapi.uptime_ms();
    uint64_t spin_id = kapi.task_create("SPIN", spinner, 0);
    for (uint64_t id = 1; id <= 3; id++) kapi.task_create("WORKER", worker, (void *)(uintptr_t)id);

    for (int i = 0; g_done < 3 && i < 500; i++) kapi.sleep_ms(10);   /* Haupt-Task = Leerlauf */
    uint64_t t1 = kapi.uptime_ms();

    int ok = spin_id && g_done == 3 && g_spin > 0;
    int killed = kapi.task_kill(spin_id) == 0;
    uint64_t spin_a = g_spin;
    kapi.sleep_ms(50);   /* Scheduler raeumt die tote Task auf */
    ok = ok && killed && g_spin == spin_a;   /* nach dem Beenden darf der Zaehler nicht mehr laufen */
    report("SCHEDULER TEST", ok);
    con_puts("  LAUFZEIT ");
    con_dec(t1 - t0);
    con_puts(" MS, PMM FREI ");
    con_dec(pmm_free_page_count());
    con_puts(" SEITEN\n");
}

/* ------------------------------------------------------------------ */
/* Kernel                                                              */
/* ------------------------------------------------------------------ */
void kmain(BootInfo *bi);
void kmain(BootInfo *bi) {
    serial_init();

    if (!bi || bi->magic != BIYOS_BOOTINFO_MAGIC) {
        for (const char *s = "BIYOS: BootInfo ungueltig\r\n"; *s; s++) serial_putc(*s);
        kernel_halt();
    }

    if (bi->fb_base && (bi->fb_format == FB_FORMAT_RGBX || bi->fb_format == FB_FORMAT_BGRX)) {
        fb = (uint8_t *)(uintptr_t)bi->fb_base;
        fb_w = bi->fb_width;
        fb_h = bi->fb_height;
        fb_pitch = bi->fb_pitch;
        fb_fmt = bi->fb_format;
        scale = (fb_w >= 1920) ? 3 : 2;
        cols = fb_w / CELL_W;
        rows = fb_h / CELL_H;
        con_clear();
        fill_rect(0, 0, fb_w, 6, rgb(60, 140, 255));   /* Balken oben */
    }

    con_color(120, 255, 160);
    con_puts("BIYOS KERNEL OK\n\n");

    con_color(220, 220, 220);
    con_puts("BOOTINFO V");
    con_dec(bi->version);
    con_puts("  MODE: ");
    con_puts(bi->boot_mode == BOOT_MODE_NORMAL ? "NORMAL"
             : bi->boot_mode == BOOT_MODE_NO_MODULES ? "NO MODULES" : "SELECTED");
    con_puts("\n");

    con_puts("KERNEL: ");
    con_hex(bi->kernel_base);
    con_puts(" - ");
    con_hex(bi->kernel_end);
    con_puts("\n");

    con_puts("FB: ");
    if (fb) {
        con_dec(fb_w);
        con_puts("X");
        con_dec(fb_h);
        con_puts(" PITCH ");
        con_dec(fb_pitch);
    } else {
        con_puts("NONE");
    }
    con_puts("\n");

    /* Speicherkarte: Deskriptorgroesse kommt vom Bootloader (nicht sizeof!) */
    uint64_t entries = 0, free_pages = 0;
    for (uint64_t off = 0; bi->mmap_desc_size && off + bi->mmap_desc_size <= bi->mmap_size;
         off += bi->mmap_desc_size) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)(bi->mmap + off);
        uint32_t type = *(const uint32_t *)d;
        uint64_t pages = *(const uint64_t *)(d + 24);
        entries++;
        if (type == 7) free_pages += pages;   /* EfiConventionalMemory */
    }
    con_puts("MEM: ");
    con_dec(entries);
    con_puts(" ENTRIES, ");
    con_dec(free_pages >> 8);   /* Seiten * 4096 / 1 MiB */
    con_puts(" MIB FREE\n");

    con_puts("RSDP: ");
    con_hex(bi->rsdp);
    con_puts("\n\n");

    con_puts("MODULES: ");
    con_dec(bi->module_count);
    con_puts("\n");

    const BootModule *mods = (const BootModule *)(uintptr_t)bi->modules;
    for (uint64_t i = 0; i < bi->module_count; i++) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)mods[i].addr;
        con_puts("  ");
        con_puts(mods[i].name);
        con_puts("  ");
        if (mods[i].size >= 20 && m[0] == 0xAA && m[1] == 'E' && m[2] == 'X' && m[3] == 'E') {
            for (int k = 4; k < 8; k++) con_putc((char)m[k]);   /* Typ: DRIV / STRT / PLSE */
            uint32_t ver = ((uint32_t)m[8] << 24) | ((uint32_t)m[9] << 16) | ((uint32_t)m[10] << 8) | m[11];
            con_puts("  VERSION ");
            con_dec(ver);
        } else {
            con_puts("KEIN GUELTIGER HEADER");
        }
        con_puts("\n");
    }

    /* --- CPU einrichten: GDT/TSS, IDT, flache Seitentabellen --- */
    con_color(120, 255, 160);
    con_puts("\n");
    arch_init(bi);
    mm_init(bi);
    con_puts("PMM: ");
    con_dec(pmm_free_page_count() >> 8);
    con_puts(" MIB FREI VON ");
    con_dec(pmm_total_pages() >> 8);
    con_puts(" MIB NUTZBAR\n");

    /* --- Selbsttest: Trap (int3) und eigener Handler (int 0x81) --- */
    con_color(220, 220, 220);
    idt_set_handler(0x81, test_handler);
    __asm__ volatile("int3");
    __asm__ volatile("int $0x81");
    con_puts(test_counter == 1 ? "INT TEST: OK\n" : "INT TEST: FEHLGESCHLAGEN\n");
    mm_selftest();
    sched_init();
    sched_selftest();

#ifdef BIYOS_TEST_FAULT
    con_puts("LOESE TESTFEHLER AUS...\n");
#if BIYOS_TEST_FAULT == 1
    __asm__ volatile("ud2");
#elif BIYOS_TEST_FAULT == 2
    volatile int a = 7, zero = 0;   /* a ist volatile, sonst ersetzt GCC die Division */
    volatile int q = a / zero;
    (void)q;
#elif BIYOS_TEST_FAULT == 3
    *(volatile uint64_t *)0x4000000000ULL = 1;   /* 256 GiB: nicht gemappt */
#elif BIYOS_TEST_FAULT == 4
    {   /* zugeordnet und wieder aufgehoben: der Zugriff muss einen #PF ausloesen */
        uint64_t ph = kapi.page_alloc(1);
        kapi.map(0x8000000000ULL, ph, 4096, PG_WRITE);
        kapi.unmap(0x8000000000ULL, 4096);
        *(volatile uint64_t *)0x8000000000ULL = 1;
    }
#endif
#endif

    con_color(120, 255, 160);
    con_puts("\nBIYOS HALT\n");
    kernel_halt();
}
