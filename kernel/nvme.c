/* SPDX-License-Identifier: MIT */
/*
 * nvme.c - NVMe-Treiber (Controller uebernehmen, Admin-Queue + eine I/O-Queue, Polling)
 *
 * Ablauf: Controller per CC.EN=0 anhalten (die Firmware hat ihn evtl. benutzt), Queues im
 * eigenen Speicher aufbauen, wieder starten, Identify (Controller + Namespace 1), I/O-Queue-Paar
 * anlegen, als BlockDev "nvme0" registrieren. Daten laufen ueber einen 16-KiB-Zwischenpuffer
 * (PRP1/PRP2/PRP-Liste), daher darf der Aufrufer-Puffer beliebig ausgerichtet sein.
 *
 * Keine Interrupts: Abschluesse werden per Phase-Bit abgefragt. Nur Namespace 1, nur 4-KiB-
 * Speicherseiten (CAP.MPSMIN = 0), keine Mehrfach-Queues. Der Treiber schreibt nie von selbst.
 */
#include "kernel.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#define QDEPTH        16
#define BOUNCE_PAGES  4

typedef struct {
    uint32_t d[16];   /* 64-Byte-Befehl */
} Sqe;

static volatile uint8_t *regs;
static uint32_t dstrd;
static Sqe *asq, *iosq;
static volatile uint32_t *acq, *iocq;   /* Abschlusseintraege als 4 x uint32 */
static uint32_t as_tail, ac_head, ac_phase = 1;
static uint32_t io_tail, io_head, io_phase = 1;
static uint16_t cid_counter;
static uint8_t *bounce;
static uint64_t *prp_list;
static volatile int io_lock;

static uint32_t rd32(uint32_t off) { return *(volatile uint32_t *)(regs + off); }
static void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(regs + off) = v; }
static uint64_t rd64(uint32_t off) { return ((uint64_t)rd32(off + 4) << 32) | rd32(off); }
static void wr64(uint32_t off, uint64_t v) {
    wr32(off, (uint32_t)v);
    wr32(off + 4, (uint32_t)(v >> 32));
}

static void doorbell(int qid, int is_cq, uint32_t val) {
    wr32(0x1000 + (uint32_t)(2 * qid + is_cq) * (4u << dstrd), val);
}

static void *dma_alloc(uint64_t pages) {
    uint64_t a = kapi.page_alloc(pages);
    if (!a) return 0;
    memset((void *)(uintptr_t)a, 0, pages * 4096);
    return (void *)(uintptr_t)a;
}

/* wartet bis CSTS.RDY == want; 0 = ok */
static int wait_ready(int want, uint64_t timeout_ms) {
    uint64_t t0 = timer_uptime_ms();
    for (uint64_t spin = 0; spin < 200000000ULL; spin++) {
        uint32_t st = rd32(0x1C);
        if ((int)(st & 1) == want) return 0;
        if (st & 2) return -2;   /* Controller meldet fatalen Fehler */
        if (timer_uptime_ms() - t0 > timeout_ms) break;
        __asm__ volatile("pause");
    }
    return -1;
}

/* Befehl abschicken und auf den Abschluss warten. 0 = ok, -1 Zeitueberschreitung, -2 Fehlerstatus */
static int submit(int io, Sqe *cmd) {
    Sqe *sq = io ? iosq : asq;
    volatile uint32_t *cq = io ? iocq : acq;
    uint32_t *tail = io ? &io_tail : &as_tail;
    uint32_t *head = io ? &io_head : &ac_head;
    uint32_t *phase = io ? &io_phase : &ac_phase;
    int qid = io ? 1 : 0;

    cmd->d[0] = (cmd->d[0] & 0xFFFF) | ((uint32_t)++cid_counter << 16);
    memcpy(&sq[*tail], cmd, sizeof(Sqe));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *tail = (*tail + 1) % QDEPTH;
    doorbell(qid, 0, *tail);

    uint64_t t0 = timer_uptime_ms();
    uint32_t dw3 = 0;
    for (uint64_t spin = 0;; spin++) {
        dw3 = cq[*head * 4 + 3];
        if (((dw3 >> 16) & 1) == *phase) break;
        if (spin > 100000000ULL || timer_uptime_ms() - t0 > 5000) return -1;
        __asm__ volatile("pause");
    }
    *head = (*head + 1) % QDEPTH;
    if (*head == 0) *phase ^= 1;
    doorbell(qid, 1, *head);
    return ((dw3 >> 17) & 0x7FFF) ? -2 : 0;
}

static int identify(uint32_t cns, uint32_t nsid, void *buf) {
    Sqe c;
    memset(&c, 0, sizeof c);
    uint64_t p = (uint64_t)(uintptr_t)buf;
    c.d[0] = 0x06;
    c.d[1] = nsid;
    c.d[6] = (uint32_t)p;
    c.d[7] = (uint32_t)(p >> 32);
    c.d[10] = cns;
    return submit(0, &c);
}

static void lock(void) {
    for (;;) {
        uint64_t f = irq_save();
        if (!io_lock) {
            io_lock = 1;
            irq_restore(f);
            return;
        }
        irq_restore(f);
        kapi.yield();
    }
}

static void unlock(void) { io_lock = 0; }

static int nvme_rw(BlockDev *bd, uint64_t lba, uint32_t count, void *buf, int write) {
    if (lba + count > bd->sectors) return -3;
    uint32_t ss = bd->sector_size;
    uint32_t max_chunk = (BOUNCE_PAGES * 4096) / ss;
    uint8_t *p = buf;
    lock();
    while (count) {
        uint32_t chunk = count > max_chunk ? max_chunk : count;
        uint32_t bytes = chunk * ss;
        uint32_t pages = (bytes + 4095) / 4096;
        if (write) memcpy(bounce, p, bytes);

        Sqe c;
        memset(&c, 0, sizeof c);
        uint64_t prp1 = (uint64_t)(uintptr_t)bounce;
        uint64_t prp2 = pages == 1 ? 0 : pages == 2 ? prp1 + 4096 : (uint64_t)(uintptr_t)prp_list;
        c.d[0] = write ? 0x01 : 0x02;
        c.d[1] = 1;   /* Namespace 1 */
        c.d[6] = (uint32_t)prp1;
        c.d[7] = (uint32_t)(prp1 >> 32);
        c.d[8] = (uint32_t)prp2;
        c.d[9] = (uint32_t)(prp2 >> 32);
        c.d[10] = (uint32_t)lba;
        c.d[11] = (uint32_t)(lba >> 32);
        c.d[12] = chunk - 1;
        int r = submit(1, &c);
        if (r) {
            unlock();
            return r;
        }
        if (!write) memcpy(p, bounce, bytes);
        lba += chunk;
        count -= chunk;
        p += bytes;
    }
    unlock();
    return 0;
}

static int nvme_read(BlockDev *bd, uint64_t lba, uint32_t count, void *buf) {
    return nvme_rw(bd, lba, count, buf, 0);
}

static int nvme_write(BlockDev *bd, uint64_t lba, uint32_t count, const void *buf) {
    return nvme_rw(bd, lba, count, (void *)buf, 1);
}

/* Rueckgabe: Anzahl gefundener Laufwerke (0 oder 1), negativ bei Fehler */
int nvme_init(void) {
    const PciDev *d = pci_find_class(0x01, 0x08, 0x02, 0);
    if (!d) return 0;
    pci_enable(d);

    uint64_t bar_size;
    uint64_t base = pci_bar(d, 0, &bar_size);
    if (!base) {
        con_puts("NVME: KEIN BAR0\n");
        return -1;
    }
    uint64_t map_size = (bar_size + 4095) & ~4095ULL;
    if (map_size < 0x4000) map_size = 0x4000;
    if (map_size > 0x100000) map_size = 0x100000;
    if (paging_map(base, base, map_size, PG_WRITE | PG_UC) != 0) {   /* Register: nicht cachebar */
        con_puts("NVME: BAR NICHT ABBILDBAR\n");
        return -1;
    }
    regs = (volatile uint8_t *)(uintptr_t)base;

    uint64_t cap = rd64(0x00);
    dstrd = (uint32_t)((cap >> 32) & 0xF);
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xF);
    uint64_t to_ms = ((cap >> 24) & 0xFF) * 500;
    if (to_ms < 2000) to_ms = 2000;
    if (mpsmin != 0 || (cap & 0xFFFF) < QDEPTH - 1) {
        con_puts("NVME: CONTROLLER NICHT UNTERSTUETZT (SEITENGROESSE/QUEUETIEFE)\n");
        return -1;
    }

    wr32(0x14, rd32(0x14) & ~1u);   /* Controller anhalten (Firmware hat ihn evtl. benutzt) */
    if (wait_ready(0, to_ms)) {
        con_puts("NVME: CONTROLLER LAESST SICH NICHT ANHALTEN\n");
        return -1;
    }
    wr32(0x0C, 0xFFFFFFFFu);   /* INTMS: alle Pin-Interrupts maskieren (wir pollen) */

    asq = dma_alloc(1);
    acq = dma_alloc(1);
    iosq = dma_alloc(1);
    iocq = dma_alloc(1);
    bounce = dma_alloc(BOUNCE_PAGES);
    prp_list = dma_alloc(1);
    uint8_t *ident = dma_alloc(1);
    if (!asq || !acq || !iosq || !iocq || !bounce || !prp_list || !ident) {
        con_puts("NVME: KEIN SPEICHER\n");
        return -1;
    }
    for (int i = 0; i < BOUNCE_PAGES - 1; i++) prp_list[i] = (uint64_t)(uintptr_t)bounce + 4096ULL * (uint64_t)(i + 1);

    wr32(0x24, ((QDEPTH - 1) << 16) | (QDEPTH - 1));   /* AQA */
    wr64(0x28, (uint64_t)(uintptr_t)asq);
    wr64(0x30, (uint64_t)(uintptr_t)acq);
    wr32(0x14, 1 | (6u << 16) | (4u << 20));   /* EN, NVM-Befehlssatz, 4 KiB, SQE 64 B, CQE 16 B */
    if (wait_ready(1, to_ms)) {
        con_puts("NVME: CONTROLLER WIRD NICHT BEREIT\n");
        return -1;
    }

    if (identify(1, 0, ident)) {
        con_puts("NVME: IDENTIFY CONTROLLER FEHLGESCHLAGEN\n");
        return -1;
    }
    char model[41];
    for (int i = 0; i < 40; i++) model[i] = (char)ident[24 + i];
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    if (identify(0, 1, ident)) {
        con_puts("NVME: KEIN NAMESPACE 1\n");
        return -1;
    }
    uint64_t nsze = *(uint64_t *)ident;
    uint32_t flbas = ident[26] & 0xF;
    uint32_t lbaf = *(uint32_t *)(ident + 128 + 4 * flbas);
    uint32_t lbads = (lbaf >> 16) & 0xFF;
    if (!nsze || lbads < 9 || lbads > 12) {
        con_puts("NVME: UNBRAUCHBARER NAMESPACE\n");
        return -1;
    }

    Sqe c;
    memset(&c, 0, sizeof c);   /* I/O-Completion-Queue 1 */
    c.d[0] = 0x05;
    c.d[6] = (uint32_t)(uintptr_t)iocq;
    c.d[7] = (uint32_t)((uint64_t)(uintptr_t)iocq >> 32);
    c.d[10] = ((QDEPTH - 1) << 16) | 1;
    c.d[11] = 1;   /* physisch zusammenhaengend, keine Interrupts */
    if (submit(0, &c)) {
        con_puts("NVME: I/O-CQ FEHLGESCHLAGEN\n");
        return -1;
    }
    memset(&c, 0, sizeof c);   /* I/O-Submission-Queue 1 */
    c.d[0] = 0x01;
    c.d[6] = (uint32_t)(uintptr_t)iosq;
    c.d[7] = (uint32_t)((uint64_t)(uintptr_t)iosq >> 32);
    c.d[10] = ((QDEPTH - 1) << 16) | 1;
    c.d[11] = (1u << 16) | 1;   /* gehoert zu CQ 1, physisch zusammenhaengend */
    if (submit(0, &c)) {
        con_puts("NVME: I/O-SQ FEHLGESCHLAGEN\n");
        return -1;
    }

    BlockDev *bd = kapi.malloc(sizeof(BlockDev));
    if (!bd) return -1;
    memset(bd, 0, sizeof(BlockDev));
    const char *nm = "nvme0";
    for (int i = 0; nm[i]; i++) bd->name[i] = nm[i];
    bd->sectors = nsze;
    bd->sector_size = 1u << lbads;
    bd->read = nvme_read;
    bd->write = nvme_write;
    blk_register(bd);

    con_puts("NVME: ");
    con_puts(model);
    con_puts(", ");
    con_dec((nsze * bd->sector_size) >> 20);
    con_puts(" MIB, SEKTOR ");
    con_dec(bd->sector_size);
    con_puts("\n");
    blk_scan_partitions(bd);
    return 1;
}
