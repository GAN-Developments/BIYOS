/* SPDX-License-Identifier: MIT */
/*
 * kbd.c - PS/2-Tastaturtreiber (i8042-Controller, Scancode-Satz 1 ueber die Uebersetzung)
 *
 * Interrupt IRQ 1 wird ueber den IOAPIC auf Vektor 0x21 geleitet. Klappt das nicht (kein
 * IOAPIC/keine MADT), fragt der Timer-Tick den Controller ab (kbd_tick, Polling-Modus).
 * Layouts: Deutsch (QWERTZ, Standard) und US. Zeichen, die die kleine Konsolenschrift nicht
 * kennt (Umlaute, ss), werden verworfen. Ctrl+Buchstabe liefert die Steuerzeichen 1..26.
 *
 * Noch ohne Pfeiltasten und Funktionstasten (erweiterte Codes E0 xx werden ignoriert).
 */
#include "kernel.h"

#define KBD_VECTOR 0x21
#define BUF_SIZE   256

static volatile uint8_t  buf[BUF_SIZE];
static volatile uint32_t head, tail;
static int polling;
static int layout_de = 1;
static int shift, ctrl, altgr, caps, ext;

/* Scancode-Satz 1, Make-Codes 0x00..0x39. "\0" = keine Ausgabe. */
static const char us_norm[58] =
    "\0" "\033" "1234567890-=" "\b\t" "qwertyuiop[]" "\n" "\0" "asdfghjkl;'`" "\0" "\\" "zxcvbnm,./" "\0" "*" "\0" " ";
static const char us_shift[58] =
    "\0" "\033" "!@#$%^&*()_+" "\b\t" "QWERTYUIOP{}" "\n" "\0" "ASDFGHJKL:\"~" "\0" "|" "ZXCVBNM<>?" "\0" "*" "\0" " ";
static const char de_norm[58] =
    "\0" "\033" "1234567890" "\0" "\0" "\b\t" "qwertzuiop" "\0" "+" "\n" "\0" "asdfghjkl" "\0" "\0" "^" "\0" "#" "yxcvbnm,.-" "\0" "*" "\0" " ";
static const char de_shift[58] =
    "\0" "\033" "!\"" "\0" "$%&/()=" "?" "`" "\b\t" "QWERTZUIOP" "\0" "*" "\n" "\0" "ASDFGHJKL" "\0" "\0" "\0" "\0" "'" "YXCVBNM;:_" "\0" "*" "\0" " ";
static const char de_altgr[58] = {[8] = '{', [9] = '[', [10] = ']', [11] = '}', [12] = '\\', [16] = '@', [27] = '~'};

static void push(char c) {
    uint32_t n = (head + 1) % BUF_SIZE;
    if (n == tail) return;   /* Puffer voll: Zeichen verwerfen */
    buf[head] = (uint8_t)c;
    head = n;
}

static void handle_scancode(uint8_t sc) {
    if (sc == 0xE0) {
        ext = 1;
        return;
    }
    if (sc == 0xFA || sc == 0xFE) return;   /* ACK / Resend des Geraets */

    int brk = sc & 0x80;
    uint8_t code = sc & 0x7F;
    int e = ext;
    ext = 0;

    if (!e && (code == 0x2A || code == 0x36)) { shift = !brk; return; }
    if (code == 0x1D) { ctrl = !brk; return; }
    if (code == 0x38) { if (e) altgr = !brk; return; }   /* rechtes Alt = AltGr */
    if (!e && code == 0x3A) { if (!brk) caps = !caps; return; }
    if (brk || e) return;

    char base = 0, c = 0;
    if (code == 0x56 && layout_de) {          /* Extra-Taste links neben Y: < > | */
        c = altgr ? '|' : shift ? '>' : '<';
    } else if (code < 58) {
        base = layout_de ? de_norm[code] : us_norm[code];
        if (base >= 'a' && base <= 'z' && !altgr) {
            c = (shift ^ caps) ? (char)(base - 32) : base;
        } else if (altgr && layout_de) {
            c = de_altgr[code];
        } else if (shift) {
            c = layout_de ? de_shift[code] : us_shift[code];
        } else {
            c = base;
        }
    }
    if (!c) return;
    if (ctrl && base >= 'a' && base <= 'z') c = (char)(base & 0x1F);
    push(c);
}

static void drain(void) {
    for (int i = 0; i < 64; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) break;
        uint8_t d = inb(0x60);
        if (st & 0x20) continue;   /* Maus-Daten ignorieren */
        handle_scancode(d);
    }
}

static void kbd_irq(Regs *r) {
    (void)r;
    drain();
    lapic_eoi();
    sched_request();   /* wartende Task sofort aufwecken */
}

void kbd_tick(void) {
    if (polling) drain();
}

/* naechstes Zeichen oder -1 */
int kbd_getc(void) {
    uint64_t f = irq_save();
    int c = -1;
    if (tail != head) {
        c = buf[tail];
        tail = (tail + 1) % BUF_SIZE;
    }
    irq_restore(f);
    return c;
}

void kbd_set_layout(int de) { layout_de = de; }
int kbd_layout_de(void) { return layout_de; }

static int wait_in_empty(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) return 1;
    return 0;
}

static int wait_out_full(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 1) return 1;
    return 0;
}

static void flush(void) {
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
}

void kbd_init(void) {
    outb(0x64, 0xAD);   /* Tastaturport aus */
    outb(0x64, 0xA7);   /* Mausport aus (bleibt aus) */
    flush();

    uint8_t cfg = 0x45;
    wait_in_empty();
    outb(0x64, 0x20);   /* Konfigurationsbyte lesen */
    if (wait_out_full()) cfg = inb(0x60);
    cfg &= (uint8_t)~((1 << 0) | (1 << 1) | (1 << 4));   /* IRQs aus (vorerst), Port-1-Takt an */
    wait_in_empty();
    outb(0x64, 0x60);
    wait_in_empty();
    outb(0x60, cfg);

    outb(0x64, 0xAE);   /* Tastaturport an */
    wait_in_empty();
    outb(0x60, 0xF4);   /* Geraet: Scannen einschalten */
    if (wait_out_full()) inb(0x60);   /* ACK abholen */
    flush();

    cfg |= 1;           /* IRQ 1 einschalten */
    wait_in_empty();
    outb(0x64, 0x60);
    wait_in_empty();
    outb(0x60, cfg);

    if (ioapic_route_irq(1, KBD_VECTOR) == 0) {
        idt_set_handler(KBD_VECTOR, kbd_irq);
        con_puts("KEYBOARD: PS/2, IRQ 1 UEBER IOAPIC, LAYOUT DE\n");
    } else {
        polling = 1;
        con_puts("KEYBOARD: PS/2, POLLING (KEIN IOAPIC), LAYOUT DE\n");
    }
}
