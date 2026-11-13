/* SPDX-License-Identifier: MIT */
/*
 * usbhid.c - DRIV-Modul: xHCI-Controller und USB-Tastatur (HID-Boot-Protokoll)
 *
 * Ablauf: Das Modul sucht den ersten xHCI-Controller (PCI-Klasse 0C/03/30), uebernimmt ihn vom BIOS, startet ihn und
 * zaehlt die Root-Ports auf. An jedem Geraet laeuft: Slot holen, Adresse vergeben, Deskriptoren lesen,
 * Konfiguration setzen, den Interrupt-IN-Endpunkt einer Boot-Tastatur einrichten. Danach liest eine Task (ohne
 * Interrupts, sie fragt den Event-Ring ab) die 8-Byte-Berichte, vergleicht sie mit dem letzten Bericht und speist die
 * Tasten als PS/2-Scancodes (Set 1) per kapi.kbd_scancode in die Tastaturlogik des Kernels ein. Layout, Umschalt,
 * AltGr, Strg und Feststelltaste erledigt dort dieselbe Logik wie bei PS/2. Die Wiederholung bei gehaltener Taste
 * macht dieses Modul (USB-Tastaturen haben keine eigene).
 *
 * Hubs (USB 2) werden unterstuetzt, auch verschachtelt (bis 5 Ebenen, Route String) und mit Transaction Translator fuer
 * LS/FS-Geraete hinter HS-Hubs. Die Ports eines Hubs fragt das Modul alle 250 ms per Steuer-Transfer ab (kein
 * Status-Change-Endpunkt), so werden Anstecken/Abziehen erkannt. SuperSpeed-Hubs werden uebersprungen: ihr USB-2-Teil
 * erscheint als eigener Hub am USB-2-Root-Port und wird dort verwaltet.
 * Nicht unterstuetzt: Mause, Massenspeicher, Interrupts.
 * Das Modul bleibt geladen (module remove wird abgelehnt). Es braucht kapi >= 9.
 *
 * Gebaut wird es mit: make -C modules   (siehe modules/Makefile) -> boot/esp/BIYOS/modules/usbhid.drv
 */
#include "kapi.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static KernelApi *A;

#define RING_N   256
#define MAX_DEV  16

typedef struct {
    u64 param;
    u32 status;
    u32 control;
} Trb;

typedef struct {
    Trb *trb;
    u64  phys;
    u32  enq, cycle;
} Ring;

typedef struct {
    int  used, slot, speed;
    int  parent, pport, rootport, depth;   /* parent = Slot des Hubs (0 = Root-Port), pport = Port am Hub */
    u32  route;                            /* Route String: 4 Bit je Hub-Ebene, unterste Stufe zuerst */
    int  hub, hubports;
    u32  tried;                            /* Hub: Ports, an denen die Anmeldung schon versucht wurde */
    Ring ep0, intr;
    u8  *devctx, *inctx, *buf;       /* Seiten: Geraete-Kontext, Eingabe-Kontext, DMA-Puffer */
    u64  devctx_p, inctx_p, buf_p;
    int  kbd, dci, fails;
    u8   prev[8];
    u8   rep_key;
    u64  rep_next;
} Dev;

typedef struct {
    int valid;
    u32 code, slot, ep;
    u64 ptr;
} Done;

static uintptr_t cap, op, rt, db;
static int csz, nports, ac64;
static u64 *dcbaa;
static u64 dcbaa_p;
static Ring cmdring;
static Trb *evr;
static u64 evr_p;
static u32 ev_deq, ev_cycle;
static volatile Done g_cmd, g_xfer;
static volatile int g_psc;
static Dev g_dev[MAX_DEV];

/* ------------------------------------------------------------------ */
/* Helfer                                                              */
/* ------------------------------------------------------------------ */
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }

static inline u32 rd32(uintptr_t base, u32 off) { return *(volatile u32 *)(base + off); }
static inline void wr32(uintptr_t base, u32 off, u32 v) { *(volatile u32 *)(base + off) = v; }
static inline void wr64(uintptr_t base, u32 off, u64 v) {
    wr32(base, off, (u32)v);            /* erst unten, dann oben */
    wr32(base, off + 4, (u32)(v >> 32));
}

static void zero(void *p, u32 n) {
    volatile u8 *b = p;
    for (u32 i = 0; i < n; i++) b[i] = 0;
}

static void put_u32(u8 *p, u32 off, u32 v) { *(u32 *)(p + off) = v; }

static void say(const char *s) { A->puts(s); }

static void say_dec(u64 v) { A->dec(v); }

static void say_hex4(u32 v) {
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 12; i >= 0; i -= 4) A->putc(hx[(v >> i) & 0xF]);
}

/* eine Seite (4 KiB, genullt, 64-Byte-ausgerichtet) fuer DMA */
static u64 page(void) {
    u64 p = A->page_alloc(1);
    if (!p) return 0;
    if (!ac64 && p >= (1ULL << 32)) {
        A->page_free(p, 1);
        return 0;
    }
    zero((void *)(uintptr_t)p, 4096);
    return p;
}

static u64 now(void) { return A->uptime_ms(); }

/* ------------------------------------------------------------------ */
/* Ringe und Ereignisse                                                */
/* ------------------------------------------------------------------ */
static int ring_init(Ring *r) {
    r->phys = page();
    if (!r->phys) return -1;
    r->trb = (Trb *)(uintptr_t)r->phys;
    r->enq = 0;
    r->cycle = 1;
    r->trb[RING_N - 1].param = r->phys;                 /* Link-TRB zurueck zum Anfang, Toggle Cycle */
    r->trb[RING_N - 1].control = (6u << 10) | (1u << 1);
    return 0;
}

static void ring_push(Ring *r, u64 param, u32 status, u32 control) {
    Trb *t = &r->trb[r->enq];
    t->param = param;
    t->status = status;
    barrier();
    t->control = control | r->cycle;
    r->enq++;
    if (r->enq == RING_N - 1) {
        r->trb[RING_N - 1].control = (6u << 10) | (1u << 1) | r->cycle;   /* Link an den xHC uebergeben */
        r->enq = 0;
        r->cycle ^= 1;
    }
}

static void ring_db(u32 slot, u32 target) {
    barrier();
    wr32(db, slot * 4, target);
}

static void kbd_event(Dev *d, u32 code);

static void process_events(void) {
    int any = 0;
    for (;;) {
        Trb *e = &evr[ev_deq];
        if ((e->control & 1) != ev_cycle) break;
        barrier();
        u32 type = (e->control >> 10) & 63;
        if (type == 33) {   /* Command Completion */
            g_cmd.code = e->status >> 24;
            g_cmd.slot = e->control >> 24;
            g_cmd.ptr = e->param;
            g_cmd.valid = 1;
        } else if (type == 32) {   /* Transfer Event */
            u32 slot = e->control >> 24, ep = (e->control >> 16) & 31, code = e->status >> 24;
            Dev *kd = 0;
            for (int i = 0; i < MAX_DEV; i++)
                if (g_dev[i].used && g_dev[i].kbd && g_dev[i].slot == (int)slot && g_dev[i].dci == (int)ep) kd = &g_dev[i];
            if (kd) {
                kbd_event(kd, code);
            } else {
                g_xfer.code = code;
                g_xfer.slot = slot;
                g_xfer.ep = ep;
                g_xfer.ptr = e->param;
                g_xfer.valid = 1;
            }
        } else if (type == 34) {   /* Port Status Change */
            g_psc = 1;
        }
        ev_deq++;
        if (ev_deq == RING_N) {
            ev_deq = 0;
            ev_cycle ^= 1;
        }
        any = 1;
    }
    if (any) wr64(rt, 0x20 + 0x18, (evr_p + (u64)ev_deq * 16) | (1u << 3));   /* ERDP, EHB loeschen */
}

/* Kommando senden und auf die Fertigmeldung warten. 0 = Erfolg (Code 1), sonst Fehlercode oder -1 bei Zeitueberschreitung */
static int command(u64 param, u32 control, u32 *slot_out) {
    g_cmd.valid = 0;
    ring_push(&cmdring, param, 0, control);
    ring_db(0, 0);
    u64 t0 = now();
    while (!g_cmd.valid) {
        process_events();
        if (g_cmd.valid) break;
        if (now() - t0 > 1000) return -1;
        A->yield();
    }
    if (slot_out) *slot_out = g_cmd.slot;
    return g_cmd.code == 1 ? 0 : (int)g_cmd.code;
}

/* ------------------------------------------------------------------ */
/* Steuer-Transfers auf Endpunkt 0                                     */
/* ------------------------------------------------------------------ */
static int ctrl(Dev *d, u8 reqtype, u8 req, u16 val, u16 idx, u16 len) {
    u64 setup = (u64)reqtype | ((u64)req << 8) | ((u64)val << 16) | ((u64)idx << 32) | ((u64)len << 48);
    int in = (reqtype & 0x80) != 0;
    u32 trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(&d->ep0, setup, 8, (2u << 10) | (1u << 6) | (trt << 16));                 /* Setup, Immediate Data */
    if (len) ring_push(&d->ep0, d->buf_p, len, (3u << 10) | (in ? (1u << 16) : 0));   /* Daten */
    ring_push(&d->ep0, 0, 0, (4u << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));   /* Status, IOC */
    g_xfer.valid = 0;
    ring_db((u32)d->slot, 1);
    u64 t0 = now();
    for (;;) {
        process_events();
        if (g_xfer.valid && g_xfer.slot == (u32)d->slot && g_xfer.ep == 1) break;
        g_xfer.valid = g_xfer.valid && g_xfer.slot == (u32)d->slot && g_xfer.ep == 1;
        if (now() - t0 > 1500) return -1;
        A->yield();
    }
    return (g_xfer.code == 1 || g_xfer.code == 13) ? 0 : (int)g_xfer.code;
}

static int get_descriptor(Dev *d, u8 type, u8 index, u16 len) {
    return ctrl(d, 0x80, 6, (u16)((type << 8) | index), 0, len);
}

/* ------------------------------------------------------------------ */
/* HID: Bericht -> PS/2-Scancodes                                      */
/* ------------------------------------------------------------------ */
static const u8 sc_map[0x66] = {
    0, 0, 0, 0,
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,   /* 04 a .. 10 m */
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,   /* 11 n .. 1D z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                     /* 1E 1 .. 27 0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35,   /* 28 .. 38 */
    0x3A,                                                                           /* 39 Feststelltaste */
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,         /* 3A F1 .. 45 F12 */
    0, 0x46, 0,                                                                     /* 46 Druck, 47 Rollen, 48 Pause */
    0x52, 0x47, 0x49, 0x53, 0x4F, 0x51, 0x4D, 0x4B, 0x50, 0x48,                     /* 49 Einfg .. 52 hoch (erweitert) */
    0x45, 0x35, 0x37, 0x4A, 0x4E, 0x1C,                                             /* 53 NumLock, 54 KP/, 55 *, 56 -, 57 +, 58 KP-Enter */
    0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,               /* 59 KP1 .. 63 KP. */
    0x56, 0,                                                                        /* 64 <>| (ISO), 65 App */
};

static int is_ext(u8 usage) {
    return (usage >= 0x49 && usage <= 0x52) || usage == 0x54 || usage == 0x58;
}

static void send_usage(u8 usage, int make) {
    if (usage >= sizeof(sc_map) || !sc_map[usage]) return;
    u8 sc = sc_map[usage];
    if (is_ext(usage)) A->kbd_scancode(0xE0);
    A->kbd_scancode(make ? sc : (u8)(sc | 0x80));
}

static void send_mod(int bit, int make) {
    static const u8 code[8] = {0x1D, 0x2A, 0x38, 0x5B, 0x1D, 0x36, 0x38, 0x5C};
    static const u8 ext[8] = {0, 0, 0, 1, 1, 0, 1, 1};
    if (ext[bit]) A->kbd_scancode(0xE0);
    A->kbd_scancode(make ? code[bit] : (u8)(code[bit] | 0x80));
}

static int no_repeat(u8 usage) {
    return usage == 0x39 || usage == 0x47 || usage == 0x53;   /* Feststell-, Rollen-, Num-Taste */
}

static void handle_report(Dev *d, const u8 *r) {
    for (int b = 0; b < 8; b++) {   /* Umschalttasten */
        int was = (d->prev[0] >> b) & 1, is = (r[0] >> b) & 1;
        if (was != is) send_mod(b, is);
    }
    for (int i = 2; i < 8; i++) {   /* losgelassene Tasten */
        u8 k = d->prev[i];
        if (k < 4) continue;
        int still = 0;
        for (int j = 2; j < 8; j++)
            if (r[j] == k) still = 1;
        if (!still) {
            send_usage(k, 0);
            if (d->rep_key == k) d->rep_key = 0;
        }
    }
    for (int i = 2; i < 8; i++) {   /* neu gedrueckte Tasten */
        u8 k = r[i];
        if (k < 4) continue;
        int had = 0;
        for (int j = 2; j < 8; j++)
            if (d->prev[j] == k) had = 1;
        if (!had) {
            send_usage(k, 1);
            if (!no_repeat(k)) {
                d->rep_key = k;
                d->rep_next = now() + 500;
            }
        }
    }
    for (int i = 0; i < 8; i++) d->prev[i] = r[i];
}

static void queue_report(Dev *d) {
    ring_push(&d->intr, d->buf_p + 2048, 8, (1u << 10) | (1u << 5) | (1u << 2));   /* Normal, IOC, ISP */
    ring_db((u32)d->slot, (u32)d->dci);
}

static void kbd_event(Dev *d, u32 code) {
    if (code == 1 || code == 13) {
        d->fails = 0;
        handle_report(d, d->buf + 2048);
        queue_report(d);
    } else if (++d->fails < 5) {
        queue_report(d);   /* einmalige Fehler (z.B. Babble) ueberspringen */
    } else if (d->fails == 5) {
        say("USB: TASTATUR MELDET FEHLER ");
        say_dec(code);
        say(" - AUFGEGEBEN\n");
    }
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */
static u32 portsc_off(int port) { return 0x400 + 0x10u * (u32)(port - 1); }

#define PORT_KEEP (~0x00FE0002u)   /* RW1C-Bits und PED beim Zurueckschreiben nicht mitschicken */

static void dev_free(Dev *d) {
    if (d->devctx_p) A->page_free(d->devctx_p, 1);
    if (d->inctx_p) A->page_free(d->inctx_p, 1);
    if (d->ep0.phys) A->page_free(d->ep0.phys, 1);
    if (d->intr.phys) A->page_free(d->intr.phys, 1);
    if (d->buf_p) A->page_free(d->buf_p, 1);
    zero(d, sizeof(Dev));
}

static int dci_of(u8 epaddr) { return (epaddr & 15) * 2 + ((epaddr & 0x80) ? 1 : 0); }

static void say_path(const Dev *d) {   /* Root-Port, dann der Reihe nach die Hub-Ports: "1.3.2" */
    say_dec((u64)d->rootport);
    for (int i = 0; i < d->depth; i++) {
        A->putc('.');
        say_dec((u64)((d->route >> (4 * i)) & 15));
    }
}

static const char *speed_name(int s) {
    return s == 1 ? "FS" : s == 2 ? "LS" : s == 3 ? "HS" : s == 4 ? "SS" : "?";
}

static Dev *find_child(int hub_slot, int pport) {
    for (int i = 0; i < MAX_DEV; i++)
        if (g_dev[i].used && g_dev[i].parent == hub_slot && g_dev[i].pport == pport) return &g_dev[i];
    return 0;
}

/* Geraet samt allen Geraeten dahinter abmelden und freigeben */
static void remove_dev(Dev *d) {
    int slot = d->slot;
    for (int i = 0; i < MAX_DEV; i++)
        if (g_dev[i].used && g_dev[i].parent == slot && &g_dev[i] != d) remove_dev(&g_dev[i]);
    if (slot) {
        command(0, (10u << 10) | ((u32)slot << 24), 0);   /* Disable Slot */
        dcbaa[slot] = 0;
    }
    dev_free(d);
}

static int hub_setup(Dev *d, u8 proto);
static void hub_poll(Dev *h);

/* Geraet an einem Root-Port (parent == 0) oder Hub-Port anmelden, adressieren und einrichten */
static int setup_device(Dev *parent, int pport, int speed, int rootport) {
    if (parent && parent->depth > 4) {
        say("USB: HUBS ZU TIEF VERSCHACHTELT\n");
        return -1;
    }
    Dev *d = 0;
    for (int i = 0; i < MAX_DEV; i++)
        if (!g_dev[i].used) {
            d = &g_dev[i];
            break;
        }
    if (!d) {
        say("USB: ZU VIELE GERAETE\n");
        return -1;
    }
    zero(d, sizeof(Dev));
    d->parent = parent ? parent->slot : 0;
    d->pport = pport;
    d->rootport = rootport;
    d->depth = parent ? parent->depth + 1 : 0;
    d->route = parent ? (parent->route | ((u32)(pport & 15) << (4 * parent->depth))) : 0;
    d->speed = speed;
    d->devctx_p = page();
    d->inctx_p = page();
    d->buf_p = page();
    if (!d->devctx_p || !d->inctx_p || !d->buf_p || ring_init(&d->ep0) != 0) {
        say("USB: KEIN SPEICHER\n");
        dev_free(d);
        return -1;
    }
    d->devctx = (u8 *)(uintptr_t)d->devctx_p;
    d->inctx = (u8 *)(uintptr_t)d->inctx_p;
    d->buf = (u8 *)(uintptr_t)d->buf_p;

    u32 slot = 0;
    if (command(0, 9u << 10, &slot) != 0 || !slot || slot > 255) {
        say("USB: KEIN SLOT\n");
        dev_free(d);
        return -1;
    }
    d->slot = (int)slot;
    d->used = 1;
    dcbaa[slot] = d->devctx_p;

    u32 mps = speed == 4 ? 512 : speed == 3 ? 64 : 8;   /* SS / HS / LS+FS (FS wird unten ggf. korrigiert) */
    put_u32(d->inctx, 4, 3);                              /* Add: Slot + EP0 */
    put_u32(d->inctx, 1 * (u32)csz + 0, (1u << 27) | ((u32)speed << 20) | d->route);
    put_u32(d->inctx, 1 * (u32)csz + 4, (u32)rootport << 16);
    if (parent && parent->speed == 3 && (speed == 1 || speed == 2))   /* LS/FS hinter einem HS-Hub: Transaction Translator */
        put_u32(d->inctx, 1 * (u32)csz + 8, (u32)parent->slot | ((u32)pport << 8));
    u8 *e0 = d->inctx + 2 * csz;
    put_u32(e0, 4, (3u << 1) | (4u << 3) | (mps << 16));
    put_u32(e0, 8, (u32)d->ep0.phys | 1);
    put_u32(e0, 12, (u32)(d->ep0.phys >> 32));
    put_u32(e0, 16, 8);
    A->sleep_ms(10);
    int r = command(d->inctx_p, (11u << 10) | (slot << 24), 0);
    if (r != 0) {
        say("USB: ADRESSVERGABE FEHLGESCHLAGEN (");
        say_dec((u64)(u32)r);
        say(")\n");
        goto fail;
    }

    if (get_descriptor(d, 1, 0, 8) != 0) {
        say("USB: GERAETE-DESKRIPTOR NICHT LESBAR\n");
        goto fail;
    }
    if (speed == 1 || speed == 2) {   /* LS/FS: echte Paketgroesse von EP0 uebernehmen */
        u32 real = d->buf[7];
        if (real >= 8 && real != mps) {
            zero(d->inctx, 4096);
            put_u32(d->inctx, 4, 2);   /* Add: nur EP0 */
            put_u32(d->inctx + 2 * csz, 4, (3u << 1) | (4u << 3) | (real << 16));
            command(d->inctx_p, (13u << 10) | (slot << 24), 0);
        }
    }
    if (get_descriptor(d, 1, 0, 18) != 0) {
        say("USB: GERAETE-DESKRIPTOR NICHT LESBAR\n");
        goto fail;
    }
    u8 dclass = d->buf[4], dproto = d->buf[6];
    u16 vid = (u16)(d->buf[8] | (d->buf[9] << 8)), pid = (u16)(d->buf[10] | (d->buf[11] << 8));
    if (dclass == 9) {
        if (hub_setup(d, dproto) != 0) goto fail;
        return 0;
    }

    if (get_descriptor(d, 2, 0, 9) != 0) goto fail;
    u16 total = (u16)(d->buf[2] | (d->buf[3] << 8));
    if (total > 1000) total = 1000;
    if (total > 9 && get_descriptor(d, 2, 0, total) != 0) goto fail;
    u8 cfgval = d->buf[5];

    /* Boot-Tastatur suchen: Schnittstelle Klasse 3, Unterklasse 1, Protokoll 1 mit Interrupt-IN-Endpunkt */
    int iface = -1;
    u8 epaddr = 0, epattr = 0, interval = 0;
    u16 epmps = 0;
    int in_kbd = 0;
    for (u32 p = 0; p + 2 <= total && d->buf[p] >= 2;) {
        u8 len = d->buf[p], type = d->buf[p + 1];
        if (type == 4 && p + 9 <= total) {
            in_kbd = d->buf[p + 5] == 3 && d->buf[p + 6] == 1 && d->buf[p + 7] == 1;
            if (in_kbd && iface < 0) iface = d->buf[p + 2];
            else if (iface >= 0) in_kbd = 0;
        } else if (type == 5 && in_kbd && !epaddr && p + 7 <= total) {
            if ((d->buf[p + 2] & 0x80) && (d->buf[p + 3] & 3) == 3) {
                epaddr = d->buf[p + 2];
                epattr = d->buf[p + 3];
                epmps = (u16)(d->buf[p + 4] | (d->buf[p + 5] << 8));
                interval = d->buf[p + 6];
            }
        }
        p += len;
    }
    (void)epattr;
    if (iface < 0 || !epaddr) {
        say("USB: GERAET ");
        say_hex4(vid);
        A->putc(':');
        say_hex4(pid);
        say(" AN PORT ");
        say_path(d);
        say(" (KEINE TASTATUR)\n");
        return 0;
    }

    if (ctrl(d, 0x00, 9, cfgval, 0, 0) != 0) {   /* SET_CONFIGURATION */
        say("USB: KONFIGURATION FEHLGESCHLAGEN\n");
        goto fail;
    }
    ctrl(d, 0x21, 0x0B, 0, (u16)iface, 0);   /* SET_PROTOCOL: Boot (Fehler sind unkritisch) */
    ctrl(d, 0x21, 0x0A, 0, (u16)iface, 0);   /* SET_IDLE: nur bei Aenderung berichten */

    int dci = dci_of(epaddr);
    if (ring_init(&d->intr) != 0) goto fail;
    u32 ival;
    if (speed >= 3) {
        ival = interval ? (u32)interval - 1 : 0;   /* HS/SS: 2^(n) Mikroframes */
        if (ival > 15) ival = 15;
    } else {   /* LS/FS: Millisekunden -> 2^n * 125 us */
        u32 us8 = (u32)(interval ? interval : 1) * 8;
        ival = 3;
        while (ival < 10 && (1u << (ival + 1)) <= us8) ival++;
    }
    zero(d->inctx, 4096);
    put_u32(d->inctx, 4, 1u | (1u << dci));
    for (int i = 0; i < 32; i++) d->inctx[csz + i] = d->devctx[i];   /* aktuellen Slot-Kontext uebernehmen */
    u32 s0 = *(u32 *)(d->inctx + csz);
    put_u32(d->inctx, (u32)csz, (s0 & 0x07FFFFFFu) | ((u32)dci << 27));
    u8 *ec = d->inctx + (u32)(dci + 1) * (u32)csz;
    put_u32(ec, 0, ival << 16);
    put_u32(ec, 4, (3u << 1) | (7u << 3) | ((u32)(epmps & 0x7FF) << 16));
    put_u32(ec, 8, (u32)d->intr.phys | 1);
    put_u32(ec, 12, (u32)(d->intr.phys >> 32));
    put_u32(ec, 16, 8 | (8u << 16));
    r = command(d->inctx_p, (12u << 10) | (slot << 24), 0);
    if (r != 0) {
        say("USB: ENDPUNKT-KONFIGURATION FEHLGESCHLAGEN (");
        say_dec((u64)(u32)r);
        say(")\n");
        goto fail;
    }
    d->kbd = 1;
    d->dci = dci;
    queue_report(d);
    say("USB: TASTATUR AN PORT ");
    say_path(d);
    say(" (");
    say_hex4(vid);
    A->putc(':');
    say_hex4(pid);
    say(", ");
    say(speed_name(speed));
    if (parent && parent->speed == 3 && (speed == 1 || speed == 2)) say(" UEBER TT");
    say(")\n");
    return 0;

fail:
    if (d->slot) {
        command(0, (10u << 10) | ((u32)d->slot << 24), 0);   /* Slot freigeben */
        dcbaa[d->slot] = 0;
    }
    dev_free(d);
    return -1;
}


/* ------------------------------------------------------------------ */
/* Hubs (USB 2): Ports per Steuer-Transfers abfragen                   */
/* ------------------------------------------------------------------ */
static int port_status(Dev *h, int p, u32 *ps, u32 *pc) {
    if (ctrl(h, 0xA3, 0, 0, (u16)p, 4) != 0) return -1;   /* GET_STATUS (Port) */
    *ps = (u32)(h->buf[0] | (h->buf[1] << 8));
    *pc = (u32)(h->buf[2] | (h->buf[3] << 8));
    return 0;
}

static void clear_feature(Dev *h, int p, int feature) {
    ctrl(h, 0x23, 1, (u16)feature, (u16)p, 0);
}

static int hub_setup(Dev *d, u8 proto) {
    if (d->speed == 4) {   /* SuperSpeed-Hub: dessen USB-2-Teil erscheint als eigener Hub am USB-2-Root-Port */
        say("USB: SUPERSPEED-HUB AN PORT ");
        say_path(d);
        say(" UEBERSPRUNGEN (USB-2-TEIL WIRD EINZELN ERKANNT)\n");
        return 0;
    }
    if (ctrl(d, 0xA0, 6, 0x2900, 0, 9) != 0) return -1;   /* Hub-Deskriptor */
    u32 n = d->buf[2], chars = (u32)(d->buf[3] | (d->buf[4] << 8)), pwr = d->buf[5];
    if (!n || n > 15) return -1;
    if (get_descriptor(d, 2, 0, 9) != 0) return -1;
    u8 cfgval = d->buf[5];
    if (ctrl(d, 0x00, 9, cfgval, 0, 0) != 0) return -1;

    zero(d->inctx, 4096);   /* Hub-Felder im Slot-Kontext setzen */
    put_u32(d->inctx, 4, 1);
    for (int i = 0; i < 32; i++) d->inctx[csz + i] = d->devctx[i];
    u32 s0 = *(u32 *)(d->inctx + csz), s1 = *(u32 *)(d->inctx + csz + 4), s2 = *(u32 *)(d->inctx + csz + 8);
    s0 |= 1u << 26;                      /* Hub */
    if (proto == 2) s0 |= 1u << 25;      /* MTT: mehrere Transaction Translators */
    s1 = (s1 & 0x00FFFFFFu) | (n << 24); /* Anzahl Ports */
    s2 = (s2 & ~(3u << 16)) | (((chars >> 5) & 3u) << 16);   /* TT Think Time */
    put_u32(d->inctx, (u32)csz, s0);
    put_u32(d->inctx, (u32)csz + 4, s1);
    put_u32(d->inctx, (u32)csz + 8, s2);
    int r = command(d->inctx_p, (12u << 10) | ((u32)d->slot << 24), 0);
    if (r != 0) {
        say("USB: HUB-KONFIGURATION FEHLGESCHLAGEN (");
        say_dec((u64)(u32)r);
        say(")\n");
        return -1;
    }
    d->hub = 1;
    d->hubports = (int)n;
    for (u32 p = 1; p <= n; p++) ctrl(d, 0x23, 3, 8, (u16)p, 0);   /* SET_FEATURE PORT_POWER */
    u32 wait = pwr * 2 + 20;
    A->sleep_ms(wait < 100 ? 100 : wait);
    say("USB: HUB AN PORT ");
    say_path(d);
    say(" (");
    say_dec(n);
    say(" PORTS, ");
    say(speed_name(d->speed));
    say(")\n");
    hub_poll(d);
    return 0;
}

/* Port am Hub zuruecksetzen und das Geraet dahinter anmelden */
static void hub_enum_port(Dev *h, int p) {
    if (ctrl(h, 0x23, 3, 4, (u16)p, 0) != 0) return;   /* SET_FEATURE PORT_RESET */
    u32 ps = 0, pc = 0;
    u64 t0 = now();
    for (;;) {
        A->sleep_ms(20);
        if (port_status(h, p, &ps, &pc) != 0) return;
        if (pc & 0x10) break;   /* Reset beendet */
        if (now() - t0 > 600) {
            say("USB: PORT-RESET AM HUB ZEITUEBERSCHREITUNG\n");
            return;
        }
    }
    clear_feature(h, p, 20);   /* C_PORT_RESET */
    clear_feature(h, p, 16);   /* C_PORT_CONNECTION */
    if (!(ps & 2)) {
        say("USB: PORT AM HUB WURDE NICHT AKTIV\n");
        return;
    }
    int speed = (ps & (1u << 9)) ? 2 : (ps & (1u << 10)) ? 3 : 1;   /* LS / HS / FS */
    A->sleep_ms(20);
    setup_device(h, p, speed, h->rootport);
}

static void hub_poll(Dev *h) {
    for (int p = 1; p <= h->hubports && h->used; p++) {
        u32 ps, pc;
        if (port_status(h, p, &ps, &pc) != 0) continue;
        if (pc & 0x01) clear_feature(h, p, 16);   /* C_PORT_CONNECTION */
        if (pc & 0x02) clear_feature(h, p, 17);   /* C_PORT_ENABLE */
        if (pc & 0x04) clear_feature(h, p, 18);   /* C_PORT_SUSPEND */
        if (pc & 0x08) clear_feature(h, p, 19);   /* C_PORT_OVER_CURRENT */
        if (pc & 0x10) clear_feature(h, p, 20);   /* C_PORT_RESET */
        Dev *child = find_child(h->slot, p);
        if (!(ps & 1)) {
            h->tried &= ~(1u << p);
            if (child) {
                say("USB: GERAET AN PORT ");
                say_path(child);
                say(" ENTFERNT\n");
                remove_dev(child);
            }
        } else if (!child && !(h->tried & (1u << p))) {
            h->tried |= 1u << p;
            hub_enum_port(h, p);
        }
    }
}

/* Root-Port: Strom, ggf. Reset, Geschwindigkeit bestimmen, dann anmelden */
static int enumerate(int port) {
    u32 off = portsc_off(port);
    u32 ps = rd32(op, off);
    if (!(ps & 1)) return -1;
    if (!(ps & (1u << 9))) {   /* Port nicht bestromt */
        wr32(op, off, (ps & PORT_KEEP) | (1u << 9));
        A->sleep_ms(30);
        ps = rd32(op, off);
    }
    if (!(ps & 2)) {   /* USB-2-Port: zuruecksetzen (USB-3-Ports sind nach dem Link-Aufbau meist schon aktiv) */
        wr32(op, off, (ps & PORT_KEEP) | (1u << 4));
        u64 t0 = now();
        while (now() - t0 < 500) {
            ps = rd32(op, off);
            if (ps & (1u << 21)) break;
            A->sleep_ms(10);
        }
        wr32(op, off, (rd32(op, off) & PORT_KEEP) | (1u << 21));   /* PRC loeschen */
        A->sleep_ms(20);
        ps = rd32(op, off);
    }
    if (!(ps & 2) || !(ps & 1)) return -1;
    int speed = (int)((ps >> 10) & 15);
    if (!speed) return -1;
    return setup_device(0, port, speed, port);
}

static void scan_ports(void) {
    for (int p = 1; p <= nports; p++) {
        u32 off = portsc_off(p);
        u32 ps = rd32(op, off);
        if (ps & (1u << 17)) wr32(op, off, (ps & PORT_KEEP) | (1u << 17));   /* CSC quittieren */
        Dev *have = 0;
        for (int i = 0; i < MAX_DEV; i++)
            if (g_dev[i].used && g_dev[i].parent == 0 && g_dev[i].rootport == p) have = &g_dev[i];
        if (!(ps & 1)) {
            if (have) {   /* abgezogen (samt allem, was dahinter hing) */
                say("USB: GERAET AN PORT ");
                say_path(have);
                say(" ENTFERNT\n");
                remove_dev(have);
            }
        } else if (!have) {
            enumerate(p);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Controller                                                          */
/* ------------------------------------------------------------------ */
static int wait_clear(uintptr_t base, u32 off, u32 mask, u32 ms) {
    u64 t0 = now();
    while (rd32(base, off) & mask) {
        if (now() - t0 > ms) return -1;
        A->sleep_ms(1);
    }
    return 0;
}

static int wait_set(uintptr_t base, u32 off, u32 mask, u32 ms) {
    u64 t0 = now();
    while (!(rd32(base, off) & mask)) {
        if (now() - t0 > ms) return -1;
        A->sleep_ms(1);
    }
    return 0;
}

static int xhci_start(const PciDev *pd) {
    u64 size = 0;
    u64 bar = A->pci_bar(pd, 0, &size);
    if (!bar || !size) return -1;
    A->pci_enable(pd);
    if (bar >= (1ULL << 32)) {   /* oberhalb der 4 GiB Identity-Abbildung: uncached einblenden */
        u64 sz = (size + 4095) & ~4095ULL;
        if (A->map(bar, bar, sz, 0x1B) != 0) return -1;
    }
    cap = (uintptr_t)bar;
    u32 caplen = rd32(cap, 0) & 0xFF;
    u32 hcs1 = rd32(cap, 4), hcs2 = rd32(cap, 8), hcc = rd32(cap, 0x10);
    if (rd32(cap, 0) == 0xFFFFFFFFu) return -1;
    op = cap + caplen;
    db = cap + (rd32(cap, 0x14) & ~3u);
    rt = cap + (rd32(cap, 0x18) & ~0x1Fu);
    ac64 = hcc & 1;
    csz = (hcc & 4) ? 64 : 32;
    nports = (int)(hcs1 >> 24);
    u32 maxslots = hcs1 & 0xFF;

    /* BIOS-Besitz abgeben lassen (USB Legacy Support) */
    u32 xecp = (hcc >> 16) & 0xFFFF;
    if (xecp) {
        u32 eo = xecp * 4;
        for (int guard = 0; guard < 64; guard++) {
            u32 reg = rd32(cap, eo);
            if ((reg & 0xFF) == 1) {
                if (reg & (1u << 16)) {
                    wr32(cap, eo, reg | (1u << 24));
                    wait_clear(cap, eo, 1u << 16, 1000);
                }
                u32 ctl = rd32(cap, eo + 4);
                wr32(cap, eo + 4, ctl & ~((1u << 0) | (1u << 4) | (1u << 13) | (1u << 14) | (1u << 15)));
            }
            u32 next = (reg >> 8) & 0xFF;
            if (!next) break;
            eo += next * 4;
        }
    }

    u32 cmd = rd32(op, 0);   /* anhalten und zuruecksetzen */
    if (cmd & 1) {
        wr32(op, 0, cmd & ~1u);
        if (wait_set(op, 4, 1, 200) != 0) return -1;
    }
    wr32(op, 0, rd32(op, 0) | 2);
    if (wait_clear(op, 0, 2, 1000) != 0 || wait_clear(op, 4, 1u << 11, 1000) != 0) return -1;

    wr32(op, 0x38, maxslots);   /* CONFIG */
    dcbaa_p = page();
    if (!dcbaa_p) return -1;
    dcbaa = (u64 *)(uintptr_t)dcbaa_p;
    u32 nscratch = ((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F);
    if (nscratch) {
        u64 arr = page();
        if (!arr) return -1;
        u64 *a = (u64 *)(uintptr_t)arr;
        for (u32 i = 0; i < nscratch && i < 512; i++) {
            a[i] = page();
            if (!a[i]) return -1;
        }
        dcbaa[0] = arr;
    }
    wr64(op, 0x30, dcbaa_p);
    if (ring_init(&cmdring) != 0) return -1;
    wr64(op, 0x18, cmdring.phys | 1);

    evr_p = page();
    u64 erst = page();
    if (!evr_p || !erst) return -1;
    evr = (Trb *)(uintptr_t)evr_p;
    ((u64 *)(uintptr_t)erst)[0] = evr_p;
    ((u32 *)(uintptr_t)erst)[2] = RING_N;
    ev_deq = 0;
    ev_cycle = 1;
    wr32(rt, 0x20 + 0x08, 1);                  /* ERSTSZ */
    wr64(rt, 0x20 + 0x18, evr_p);              /* ERDP   */
    wr64(rt, 0x20 + 0x10, erst);               /* ERSTBA */

    wr32(op, 0, rd32(op, 0) | 1);   /* Run (ohne Interrupts: wir fragen den Event-Ring ab) */
    if (wait_clear(op, 4, 1, 200) != 0) return -1;
    A->sleep_ms(100);                /* Ports brauchen einen Moment, bis Geraete erkannt sind */
    return 0;
}

static void usb_task(void *arg) {
    (void)arg;
    const PciDev *pd = A->pci_find_class(0x0C, 0x03, 0x30, 0);
    if (!pd) {
        say("USB: KEIN XHCI-CONTROLLER GEFUNDEN\n");
        A->task_exit();
    }
    if (xhci_start(pd) != 0) {
        say("USB: XHCI-CONTROLLER LIESS SICH NICHT STARTEN\n");
        A->task_exit();
    }
    say("USB: XHCI, ");
    say_dec((u64)nports);
    say(" PORTS\n");
    scan_ports();

    u64 next_hub_poll = 0;
    for (;;) {
        process_events();
        if (g_psc) {
            g_psc = 0;
            A->sleep_ms(100);   /* Prellen abwarten */
            scan_ports();
        }
        u64 t = now();
        if (t >= next_hub_poll) {   /* Hub-Ports abfragen (Anstecken/Abziehen hinter Hubs) */
            next_hub_poll = t + 250;
            for (int i = 0; i < MAX_DEV; i++)
                if (g_dev[i].used && g_dev[i].hub) hub_poll(&g_dev[i]);
        }
        for (int i = 0; i < MAX_DEV; i++) {   /* Tastenwiederholung */
            Dev *d = &g_dev[i];
            if (d->used && d->kbd && d->rep_key && t >= d->rep_next) {
                send_usage(d->rep_key, 1);
                d->rep_next = t + 35;
            }
        }
        A->sleep_ms(5);
    }
}

__attribute__((section(".text.entry"))) int entry(KernelApi *api, int argc, char **argv) {
    (void)argv;
    if (argc < 0) return 1;   /* bleibt geladen */
    A = api;
    if (api->version < 9) {
        api->puts("USBHID: BRAUCHT KAPI V9\n");
        return 1;
    }
    if (!api->task_create("USB", usb_task, 0)) return 1;
    return 0;
}
