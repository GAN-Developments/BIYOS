/* SPDX-License-Identifier: MIT */
/*
 * acpi.c - minimaler ACPI-Teil: MADT lesen und ISA-Interrupts ueber den IOAPIC routen
 *
 * Wir brauchen von ACPI vorerst nur die MADT: Adresse und GSI-Basis des (ersten) IOAPIC sowie
 * die Interrupt-Source-Overrides (z.B. wenn ISA-IRQ 0 auf GSI 2 liegt). Das alte PIC ist
 * stillgelegt (sched_init), Geraete-Interrupts laufen also ueber IOAPIC -> Local APIC.
 */
#include "kernel.h"

typedef struct {
    char     sig[8];
    uint8_t  chk;
    char     oem[6];
    uint8_t  rev;
    uint32_t rsdt;
    uint32_t len;
    uint64_t xsdt;
    uint8_t  xchk;
    uint8_t  res[3];
} __attribute__((packed)) Rsdp;

typedef struct {
    char     sig[4];
    uint32_t len;
    uint8_t  rev;
    uint8_t  chk;
    char     oem[6];
    char     oemtab[8];
    uint32_t oemrev;
    uint32_t creator;
    uint32_t creatorrev;
} __attribute__((packed)) SdtHeader;

#define MAX_ISO 16

static struct {
    uint8_t  irq;
    uint32_t gsi;
    uint16_t flags;
} iso[MAX_ISO];
static int iso_count;

static int have_ioapic;
static uint32_t ioapic_phys, ioapic_gsi_base;
static volatile uint32_t *ioapic;

static int sig_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static int checksum_ok(const void *p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + ((const uint8_t *)p)[i]);
    return s == 0;
}

static const SdtHeader *find_table(const Rsdp *r, const char *sig) {
    int use_x = r->rev >= 2 && r->xsdt;
    const SdtHeader *root = (const SdtHeader *)(uintptr_t)(use_x ? r->xsdt : (uint64_t)r->rsdt);
    if (!root || root->len < sizeof(SdtHeader)) return 0;
    uint32_t n = (root->len - (uint32_t)sizeof(SdtHeader)) / (use_x ? 8 : 4);
    if (n > 256) n = 256;
    const uint8_t *e = (const uint8_t *)root + sizeof(SdtHeader);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t a = use_x ? *(const uint64_t *)(e + i * 8) : *(const uint32_t *)(e + i * 4);
        const SdtHeader *h = (const SdtHeader *)(uintptr_t)a;
        if (h && sig_eq(h->sig, sig, 4)) return h;
    }
    return 0;
}

/* 0 = ok, sonst negativ */
int acpi_init(uint64_t rsdp_addr) {
    const Rsdp *r = (const Rsdp *)(uintptr_t)rsdp_addr;
    if (!r || !sig_eq(r->sig, "RSD PTR ", 8) || !checksum_ok(r, 20)) {
        con_puts("ACPI: KEIN GUELTIGER RSDP\n");
        return -1;
    }
    const SdtHeader *m = find_table(r, "APIC");   /* die MADT heisst "APIC" */
    if (!m || m->len < 44 || !checksum_ok(m, m->len)) {
        con_puts("ACPI: KEINE MADT\n");
        return -2;
    }
    const uint8_t *p = (const uint8_t *)m + 44;   /* nach Kopf, LAPIC-Adresse und Flags */
    const uint8_t *end = (const uint8_t *)m + m->len;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2) break;
        if (type == 1 && len >= 12 && !have_ioapic) {            /* IOAPIC */
            ioapic_phys = *(const uint32_t *)(p + 4);
            ioapic_gsi_base = *(const uint32_t *)(p + 8);
            have_ioapic = 1;
        } else if (type == 2 && len >= 10 && iso_count < MAX_ISO) {   /* Interrupt Source Override */
            iso[iso_count].irq = p[3];
            iso[iso_count].gsi = *(const uint32_t *)(p + 4);
            iso[iso_count].flags = *(const uint16_t *)(p + 8);
            iso_count++;
        }
        p += len;
    }
    con_puts("ACPI: MADT OK, IOAPIC ");
    if (have_ioapic) {
        con_hex(ioapic_phys);
        con_puts(", ");
        con_dec((uint64_t)iso_count);
        con_puts(" OVERRIDES\n");
    } else {
        con_puts("NICHT GEFUNDEN\n");
    }
    return have_ioapic ? 0 : -3;
}

static void ioapic_write(uint8_t reg, uint32_t v) {
    ioapic[0] = reg;    /* IOREGSEL */
    ioapic[4] = v;      /* IOWIN (bei +0x10) */
}

static uint32_t ioapic_read(uint8_t reg) {
    ioapic[0] = reg;
    return ioapic[4];
}

/* Leitet einen ISA-Interrupt (z.B. 1 = Tastatur) als 'vector' an den Boot-Prozessor. 0 = ok */
int ioapic_route_irq(uint8_t irq, uint8_t vector) {
    if (!have_ioapic) return -1;
    uint32_t gsi = irq;
    uint16_t flags = 0;
    for (int i = 0; i < iso_count; i++) {
        if (iso[i].irq == irq) {
            gsi = iso[i].gsi;
            flags = iso[i].flags;
        }
    }
    ioapic = (volatile uint32_t *)(uintptr_t)ioapic_phys;
    uint32_t max_redir = (ioapic_read(1) >> 16) & 0xFF;
    if (gsi < ioapic_gsi_base || gsi - ioapic_gsi_base > max_redir) return -2;
    uint32_t idx = gsi - ioapic_gsi_base;

    uint32_t low = vector;                       /* Fixed, physisch, nicht maskiert */
    if ((flags & 3) == 3) low |= 1u << 13;       /* aktiv niedrig (Standard fuer ISA: hoch) */
    if (((flags >> 2) & 3) == 3) low |= 1u << 15;   /* pegelgesteuert (Standard fuer ISA: flankengesteuert) */
    ioapic_write((uint8_t)(0x10 + 2 * idx + 1), lapic_id() << 24);
    ioapic_write((uint8_t)(0x10 + 2 * idx), low);
    return 0;
}

/* Leitet eine beliebige IOAPIC-Leitung (GSI) als 'vector' an den Boot-Prozessor.
 * PCI-INTx ist pegelgesteuert und aktiv niedrig: level_low = 1. 0 = ok */
int ioapic_route_gsi(uint32_t gsi, uint8_t vector, int level_low) {
    if (!have_ioapic) return -1;
    ioapic = (volatile uint32_t *)(uintptr_t)ioapic_phys;
    uint32_t max_redir = (ioapic_read(1) >> 16) & 0xFF;
    if (gsi < ioapic_gsi_base || gsi - ioapic_gsi_base > max_redir) return -2;
    uint32_t idx = gsi - ioapic_gsi_base;
    uint32_t low = vector;
    if (level_low) low |= (1u << 13) | (1u << 15);   /* aktiv niedrig, pegelgesteuert */
    ioapic_write((uint8_t)(0x10 + 2 * idx + 1), lapic_id() << 24);
    ioapic_write((uint8_t)(0x10 + 2 * idx), low);
    return 0;
}
