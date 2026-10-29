/*
 * BIYOS Test-Kernel
 *
 * Wechselt auf einen eigenen Stack, initialisiert COM1 und den Framebuffer, gibt die
 * BootInfo aus, richtet GDT/TSS, IDT und flache Seitentabellen ein (arch.c) und fuehrt
 * einen kleinen Selbsttest der Interrupt-Behandlung durch.
 *
 * Mit  make EXTRA=-DBIYOS_TEST_FAULT=1|2|3  loest der Kernel absichtlich eine Exception
 * aus (1 = #UD, 2 = #DE, 3 = #PF), um die Fehlerausgabe zu testen.
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
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

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

void con_putc(char c) {
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

void con_puts(const char *s) {
    while (*s) con_putc(*s++);
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
/* Selbsttest der Interrupt-Behandlung                                 */
/* ------------------------------------------------------------------ */
static volatile int test_counter;

static void test_handler(Regs *r) {
    (void)r;
    test_counter++;
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
        scale = (fb_w >= 1280) ? 3 : 2;
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

    /* --- Selbsttest: Trap (int3) und eigener Handler (int 0x81) --- */
    con_color(220, 220, 220);
    idt_set_handler(0x81, test_handler);
    __asm__ volatile("int3");
    __asm__ volatile("int $0x81");
    con_puts(test_counter == 1 ? "INT TEST: OK\n" : "INT TEST: FEHLGESCHLAGEN\n");

#ifdef BIYOS_TEST_FAULT
    con_puts("LOESE TESTFEHLER AUS...\n");
#if BIYOS_TEST_FAULT == 1
    __asm__ volatile("ud2");
#elif BIYOS_TEST_FAULT == 2
    volatile int zero = 0;
    volatile int q = 1 / zero;
    (void)q;
#elif BIYOS_TEST_FAULT == 3
    *(volatile uint64_t *)0x4000000000ULL = 1;   /* 256 GiB: nicht gemappt */
#endif
#endif

    con_color(120, 255, 160);
    con_puts("\nBIYOS HALT\n");
    kernel_halt();
}
