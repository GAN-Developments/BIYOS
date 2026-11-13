/* SPDX-License-Identifier: MIT */
/*
 * ahci.c - DRIV-Modul: AHCI/SATA-Treiber (Festplatten und SSDs am SATA-Controller)
 *
 * Ablauf: Das Modul sucht alle AHCI-Controller (PCI-Klasse 01/06/01), uebernimmt sie vom BIOS, setzt sie zurueck und
 * prueft jeden vorhandenen Port auf ein ATA-Geraet. Pro Geraet wird IDENTIFY DEVICE gelesen und ein BlockDev
 * "sata0", "sata1" ... angemeldet. Danach erkennt der Kernel Partitionen und Dateisysteme (kapi.blk_scan) und bindet neue
 * FAT32-/NTFS-Partitionen ein (kapi.automount). Alles passiert synchron beim Laden, damit C:\\Start und die Laufwerke
 * beim Start der Shell schon da sind.
 *
 * Gelesen und geschrieben wird mit READ/WRITE DMA EXT (LBA48, sonst die 28-Bit-Befehle) ueber einen eigenen 64-KiB-
 * Zwischenpuffer (der Puffer des Aufrufers darf beliebig ausgerichtet sein). FLUSH CACHE folgt 200 ms nach dem letzten
 * Schreiben aus einer Hintergrund-Task ("SATAFLUSH"), nicht nach jedem Schreibaufruf.
 * Es gibt nur einen Befehls-Slot (kein NCQ) und keine Interrupts: Abschluesse werden abgefragt.
 *
 * Nicht unterstuetzt: ATAPI (CD/DVD), Port-Multiplier, Hot-Plug, NCQ, Energiesparen. Laeuft der Controller im
 * RAID-/Intel-RST-Modus statt AHCI, erscheint er nicht als AHCI-Controller und muss im BIOS auf AHCI umgestellt werden.
 *
 * Das Modul bleibt geladen (module remove wird abgelehnt). Es braucht kapi >= 6.
 * Gebaut wird es mit: make -C modules   (siehe modules/Makefile) -> boot/esp/BIYOS/modules/ahci.drv
 */
#include "kapi.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static KernelApi *A;

#define MAX_PORTS   32
#define MAX_SATA    16   /* maximal angemeldete Platten */
#define BOUNCE_PAGES 16   /* 64 KiB */

/* Port-Register (Offset ab Portbasis) */
#define PxCLB   0x00
#define PxFB    0x08
#define PxIS    0x10
#define PxIE    0x14
#define PxCMD   0x18
#define PxTFD   0x20
#define PxSIG   0x24
#define PxSSTS  0x28
#define PxSCTL  0x2C
#define PxSERR  0x30
#define PxCI    0x38

#define CMD_ST  (1u << 0)
#define CMD_SUD (1u << 1)
#define CMD_POD (1u << 2)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)

typedef struct {
    int      used;
    u32      regs_off;      /* Offset der Port-Register im HBA */
    u8      *cl, *ct, *bounce;
    u64      cl_p, ct_p, bounce_p;
    int      lba48;
    u32      secsize, maxsec;
    volatile int lock;
    int      dirty;         /* seit dem letzten FLUSH CACHE wurde geschrieben */
    u64      last_write;
    BlockDev bd;
} Port;

static uintptr_t hba;
static int s64a;
static Port g_port[MAX_SATA];
static int nports_total, nsata;

/* ------------------------------------------------------------------ */
/* Helfer                                                              */
/* ------------------------------------------------------------------ */
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }
static inline u32 rd32(uintptr_t base, u32 off) { return *(volatile u32 *)(base + off); }
static inline void wr32(uintptr_t base, u32 off, u32 v) { *(volatile u32 *)(base + off) = v; }

static u32 prd(const Port *p, u32 off) { return rd32(hba, p->regs_off + off); }
static void pwr(const Port *p, u32 off, u32 v) { wr32(hba, p->regs_off + off, v); }

static void zero(void *p, u32 n) {
    volatile u8 *b = p;
    for (u32 i = 0; i < n; i++) b[i] = 0;
}

static void copy(void *dst, const void *src, u32 n) {
    volatile u8 *d = dst;
    const volatile u8 *s = src;
    for (u32 i = 0; i < n; i++) d[i] = s[i];
}

static u64 now(void) { return A->uptime_ms(); }

static void say(const char *s) { A->puts(s); }
static void say_dec(u64 v) { A->dec(v); }

/* DMA-Speicher: zusammenhaengende Seiten, genullt, bei Controllern ohne 64-Bit-Adressen unter 4 GiB */
static u64 dma_pages(u32 pages) {
    u64 p = A->page_alloc(pages);
    if (!p) return 0;
    if (!s64a && p + (u64)pages * 4096 > (1ULL << 32)) {
        A->page_free(p, pages);
        return 0;
    }
    zero((void *)(uintptr_t)p, pages * 4096);
    return p;
}

static int wait_clear(const Port *p, u32 off, u32 mask, u32 ms) {
    u64 t0 = now();
    while (prd(p, off) & mask) {
        if (now() - t0 > ms) return -1;
        A->sleep_ms(1);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Port starten und stoppen                                            */
/* ------------------------------------------------------------------ */
static int port_stop(const Port *p) {
    u32 cmd = prd(p, PxCMD);
    if (!(cmd & (CMD_ST | CMD_CR | CMD_FRE | CMD_FR))) return 0;
    pwr(p, PxCMD, cmd & ~CMD_ST);
    if (wait_clear(p, PxCMD, CMD_CR, 500) != 0) return -1;
    pwr(p, PxCMD, prd(p, PxCMD) & ~CMD_FRE);
    return wait_clear(p, PxCMD, CMD_FR, 500);
}

static void port_start(const Port *p) {
    pwr(p, PxSERR, 0xFFFFFFFFu);
    pwr(p, PxIS, 0xFFFFFFFFu);
    pwr(p, PxCMD, prd(p, PxCMD) | CMD_FRE);
    pwr(p, PxCMD, prd(p, PxCMD) | CMD_ST);
}

/* nach einem Fehler: Port neu starten (Engine aus und an) */
static void port_recover(Port *p) {
    port_stop(p);
    port_start(p);
    wait_clear(p, PxTFD, 0x88, 2000);
}

/* ------------------------------------------------------------------ */
/* ATA-Befehl ueber Slot 0                                             */
/* ------------------------------------------------------------------ */
/* bytes > 0: Datentransfer ueber den Zwischenpuffer; write: Richtung Platte. 0 = ok */
static int ata_cmd(Port *p, u8 cmd, u8 dev, u64 lba, u32 count, int lba48cmd, int write, u32 bytes) {
    if (wait_clear(p, PxTFD, 0x88, 2000) != 0) {   /* BSY oder DRQ */
        port_recover(p);
        return -1;
    }
    u32 *h = (u32 *)p->cl;   /* Befehlskopf, Slot 0 */
    h[0] = 5u | (write ? (1u << 6) : 0u) | (bytes ? (1u << 16) : 0u);
    h[1] = 0;
    h[2] = (u32)p->ct_p;
    h[3] = (u32)(p->ct_p >> 32);

    u8 *f = p->ct;   /* Command FIS (Host zu Geraet) */
    zero(f, 64);
    f[0] = 0x27;
    f[1] = 0x80;
    f[2] = cmd;
    f[4] = (u8)lba;
    f[5] = (u8)(lba >> 8);
    f[6] = (u8)(lba >> 16);
    if (lba48cmd) {
        f[7] = dev;
        f[8] = (u8)(lba >> 24);
        f[9] = (u8)(lba >> 32);
        f[10] = (u8)(lba >> 40);
        f[12] = (u8)count;
        f[13] = (u8)(count >> 8);
    } else {
        f[7] = (u8)(dev | ((lba >> 24) & 0x0F));
        f[12] = (u8)count;
    }
    if (bytes) {
        u32 *e = (u32 *)(p->ct + 0x80);   /* PRDT-Eintrag */
        e[0] = (u32)p->bounce_p;
        e[1] = (u32)(p->bounce_p >> 32);
        e[2] = 0;
        e[3] = bytes - 1;
    }
    pwr(p, PxIS, 0xFFFFFFFFu);
    barrier();
    pwr(p, PxCI, 1);

    u64 t0 = now();
    u32 spins = 0;
    for (;;) {
        u32 is = prd(p, PxIS);
        if (is & ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27))) break;   /* TFES, HBFS, HBDS, IFS */
        if (!(prd(p, PxCI) & 1)) break;
        if (++spins < 50000) continue;   /* erst aktiv abfragen: die meisten Befehle sind in Mikrosekunden fertig,
                                          * yield() wuerde bis zum naechsten Timer-Tick (10 ms) warten */
        if (now() - t0 > 10000) {
            port_recover(p);
            return -1;
        }
        A->yield();
    }
    u32 is = prd(p, PxIS);
    u32 tfd = prd(p, PxTFD);
    pwr(p, PxIS, is);
    if ((is & ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27))) || (tfd & 0x21)) {   /* Fehler, ERR oder DF */
        port_recover(p);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* BlockDev                                                            */
/* ------------------------------------------------------------------ */
static void port_lock(Port *p) {
    while (__sync_lock_test_and_set(&p->lock, 1)) A->yield();
}

static void port_unlock(Port *p) {
    __sync_lock_release(&p->lock);
}

static int xfer(Port *p, u64 lba, u32 n, int write) {
    u8 cmd;
    if (p->lba48) cmd = write ? 0x35 : 0x25;   /* WRITE/READ DMA EXT */
    else cmd = write ? 0xCA : 0xC8;            /* WRITE/READ DMA */
    return ata_cmd(p, cmd, 0x40, lba, n, p->lba48, write, n * p->secsize);
}

static int ahci_read(BlockDev *d, u64 lba, u32 count, void *buf) {
    Port *p = d->priv;
    if (lba + count > d->sectors) return -3;
    u8 *out = buf;
    port_lock(p);
    int r = 0;
    while (count) {
        u32 n = count < p->maxsec ? count : p->maxsec;
        if (xfer(p, lba, n, 0) != 0) {
            r = -1;
            break;
        }
        copy(out, p->bounce, n * p->secsize);
        out += (u64)n * p->secsize;
        lba += n;
        count -= n;
    }
    port_unlock(p);
    return r;
}

static int ahci_write(BlockDev *d, u64 lba, u32 count, const void *buf) {
    Port *p = d->priv;
    if (lba + count > d->sectors) return -3;
    const u8 *in = buf;
    port_lock(p);
    int r = 0;
    while (count) {
        u32 n = count < p->maxsec ? count : p->maxsec;
        copy(p->bounce, in, n * p->secsize);
        if (xfer(p, lba, n, 1) != 0) {
            r = -1;
            break;
        }
        in += (u64)n * p->secsize;
        lba += n;
        count -= n;
    }
    if (r == 0) {   /* FLUSH CACHE folgt verzoegert in flush_task, nicht nach jedem Schreibaufruf */
        p->dirty = 1;
        p->last_write = now();
    }
    port_unlock(p);
    return r;
}

/* Schreibcache der Platten leeren: 200 ms nach dem letzten Schreiben (so wird nicht nach jedem kleinen Schreibaufruf
 * des Dateisystems geflusht, aber die Daten liegen kurz danach sicher auf der Platte) */
static void flush_task(void *arg) {
    (void)arg;
    for (;;) {
        A->sleep_ms(100);
        for (int i = 0; i < nports_total; i++) {
            Port *p = &g_port[i];
            if (!p->used || !p->dirty || now() - p->last_write < 200) continue;
            port_lock(p);
            if (p->dirty && now() - p->last_write >= 200) {
                if (ata_cmd(p, p->lba48 ? 0xEA : 0xE7, 0x40, 0, 0, 0, 0, 0) == 0) p->dirty = 0;
                else p->dirty = 0;   /* Fehler wurde in ata_cmd behandelt; nicht endlos wiederholen */
            }
            port_unlock(p);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Einen Port einrichten                                               */
/* ------------------------------------------------------------------ */
static int port_init(int n) {
    if (nports_total >= MAX_SATA) return -1;
    Port *p = &g_port[nports_total];
    zero(p, sizeof(Port));
    p->regs_off = 0x100 + 0x80u * (u32)n;

    if (port_stop(p) != 0) return -1;
    pwr(p, PxIE, 0);
    pwr(p, PxSERR, 0xFFFFFFFFu);
    pwr(p, PxIS, 0xFFFFFFFFu);
    pwr(p, PxCMD, prd(p, PxCMD) | CMD_SUD | CMD_POD);   /* hochfahren und bestromen */

    u64 t0 = now();   /* auf Verbindung zum Geraet warten */
    int tried_reset = 0;
    for (;;) {
        u32 det = prd(p, PxSSTS) & 0xF;
        if (det == 3) break;
        if (det == 1 && !tried_reset && now() - t0 > 200) {   /* Geraet da, aber keine Verbindung: COMRESET */
            pwr(p, PxSCTL, (prd(p, PxSCTL) & ~0xFu) | 1);
            A->sleep_ms(2);
            pwr(p, PxSCTL, prd(p, PxSCTL) & ~0xFu);
            tried_reset = 1;
        }
        if (now() - t0 > 1500) return -1;   /* kein Geraet */
        A->sleep_ms(10);
    }

    u64 pg = dma_pages(1), ctp = dma_pages(1), bp = dma_pages(BOUNCE_PAGES);
    if (!pg || !ctp || !bp) {
        say("SATA: KEIN SPEICHER\n");
        return -1;
    }
    p->cl_p = pg;
    p->cl = (u8 *)(uintptr_t)pg;
    p->ct_p = ctp;
    p->ct = (u8 *)(uintptr_t)ctp;
    p->bounce_p = bp;
    p->bounce = (u8 *)(uintptr_t)bp;
    pwr(p, PxCLB, (u32)pg);
    pwr(p, PxCLB + 4, (u32)(pg >> 32));
    pwr(p, PxFB, (u32)(pg + 1024));   /* empfangene FIS: 256-Byte-ausgerichtet hinter der Befehlsliste */
    pwr(p, PxFB + 4, (u32)((pg + 1024) >> 32));
    port_start(p);
    if (wait_clear(p, PxTFD, 0x88, 3000) != 0) {
        say("SATA: PORT ");
        say_dec((u64)n);
        say(" WIRD NICHT BEREIT\n");
        port_stop(p);
        return -1;
    }
    u32 sig = prd(p, PxSIG);
    if (sig != 0x00000101u) {   /* nur ATA-Festplatten/SSDs */
        say("SATA: PORT ");
        say_dec((u64)n);
        say(sig == 0xEB140101u ? " ATAPI (CD/DVD) NICHT UNTERSTUETZT\n" : " ANDERES GERAET NICHT UNTERSTUETZT\n");
        port_stop(p);
        return -1;
    }

    p->secsize = 512;
    p->maxsec = 128;
    if (ata_cmd(p, 0xEC, 0, 0, 0, 0, 0, 512) != 0) {   /* IDENTIFY DEVICE */
        say("SATA: IDENTIFY AN PORT ");
        say_dec((u64)n);
        say(" FEHLGESCHLAGEN\n");
        port_stop(p);
        return -1;
    }
    const u16 *id = (const u16 *)p->bounce;
    p->lba48 = (id[83] >> 10) & 1;
    u64 sectors = p->lba48 ? ((u64)id[100] | ((u64)id[101] << 16) | ((u64)id[102] << 32) | ((u64)id[103] << 48))
                           : ((u64)id[60] | ((u64)id[61] << 16));
    u16 w106 = id[106];
    if ((w106 & (1u << 14)) && !(w106 & (1u << 15)) && (w106 & (1u << 12))) {   /* logische Sektoren > 512 Bytes */
        u32 words = (u32)id[117] | ((u32)id[118] << 16);
        if (words >= 256 && words <= 4096) p->secsize = words * 2;
    }
    p->maxsec = (BOUNCE_PAGES * 4096) / p->secsize;
    if (!p->lba48 && p->maxsec > 256) p->maxsec = 256;
    if (!sectors) {
        say("SATA: GERAET AN PORT ");
        say_dec((u64)n);
        say(" MELDET 0 SEKTOREN\n");
        port_stop(p);
        return -1;
    }

    char model[41];   /* Modellname: je Wort zwei Zeichen, hohes Byte zuerst */
    for (int i = 0; i < 20; i++) {
        model[2 * i] = (char)(id[27 + i] >> 8);
        model[2 * i + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    BlockDev *bd = &p->bd;
    zero(bd, sizeof(BlockDev));
    bd->name[0] = 's';
    bd->name[1] = 'a';
    bd->name[2] = 't';
    bd->name[3] = 'a';
    bd->name[4] = (char)('0' + nsata % 10);
    bd->name[5] = 0;
    if (nsata >= 10) {
        bd->name[4] = (char)('0' + nsata / 10);
        bd->name[5] = (char)('0' + nsata % 10);
        bd->name[6] = 0;
    }
    bd->sectors = sectors;
    bd->sector_size = p->secsize;
    bd->read = ahci_read;   /* Zeiger zur Laufzeit setzen: statische Zeiger braeuchten eine Relokation */
    bd->write = ahci_write;
    bd->priv = p;
    p->used = 1;
    nports_total++;
    nsata++;

    say(bd->name);
    say(": ");
    say(model[0] ? model : "SATA-GERAET");
    say(", ");
    say_dec(sectors * p->secsize / (1024 * 1024));
    say(" MIB, SEKTOR ");
    say_dec(p->secsize);
    say(p->lba48 ? "\n" : " (LBA28)\n");
    A->blk_register(bd);
    A->blk_scan(bd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Controller                                                          */
/* ------------------------------------------------------------------ */
static int hba_start(const PciDev *pd) {
    u64 size = 0;
    u64 bar = A->pci_bar(pd, 5, &size);   /* ABAR */
    if (!bar || !size) return -1;
    A->pci_enable(pd);
    if (bar >= (1ULL << 32)) {   /* oberhalb der 4-GiB-Abbildung: uncached einblenden */
        u64 sz = (size + 4095) & ~4095ULL;
        if (A->map(bar, bar, sz, 0x1B) != 0) return -1;
    }
    hba = (uintptr_t)bar;
    u32 cap = rd32(hba, 0x00);
    if (cap == 0xFFFFFFFFu) return -1;
    s64a = (cap >> 31) & 1;

    u32 cap2 = rd32(hba, 0x24);   /* BIOS-Besitz abgeben lassen */
    if (cap2 & 1) {
        u32 bohc = rd32(hba, 0x28);
        if (bohc & 1) {
            wr32(hba, 0x28, bohc | 2);
            u64 t0 = now();
            while ((rd32(hba, 0x28) & 1) && now() - t0 < 2000) A->sleep_ms(10);
        }
    }

    wr32(hba, 0x04, rd32(hba, 0x04) | 0x80000000u);   /* AHCI-Modus */
    wr32(hba, 0x04, rd32(hba, 0x04) | 1);             /* HBA zuruecksetzen */
    u64 t0 = now();
    while (rd32(hba, 0x04) & 1) {
        if (now() - t0 > 1000) return -1;
        A->sleep_ms(1);
    }
    wr32(hba, 0x04, 0x80000000u);   /* AE ein, Interrupts aus (wir fragen ab) */
    wr32(hba, 0x08, 0xFFFFFFFFu);

    u32 pi = rd32(hba, 0x0C);
    int found = 0;
    for (int n = 0; n < 32; n++) {
        if (!((pi >> n) & 1)) continue;
        if (nports_total >= MAX_SATA) break;
        if (port_init(n) == 0) found++;
    }
    return found;
}

__attribute__((section(".text.entry"))) int entry(KernelApi *api, int argc, char **argv) {
    (void)argv;
    if (argc < 0) return 1;   /* bleibt geladen */
    A = api;
    if (api->version < 6) {
        api->puts("AHCI: BRAUCHT KAPI V6\n");
        return 1;
    }
    int controllers = 0, disks = 0;
    for (int i = 0;; i++) {
        const PciDev *pd = api->pci_find_class(0x01, 0x06, 0x01, i);
        if (!pd) break;
        controllers++;
        int r = hba_start(pd);
        if (r < 0) api->puts("AHCI: CONTROLLER LIESS SICH NICHT STARTEN\n");
        else disks += r;
    }
    if (!controllers) {
        api->puts("AHCI: KEIN AHCI-CONTROLLER GEFUNDEN\n");
        return 0;
    }
    if (disks) {
        api->automount();   /* neue Partitionen einbinden */
        api->task_create("SATAFLUSH", flush_task, 0);
    }
    else api->puts("AHCI: KEINE SATA-FESTPLATTE GEFUNDEN\n");
    return 0;
}
