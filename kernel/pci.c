/*
 * pci.c - PCI-Enumeration ueber den klassischen Konfigurationsraum (Ports 0xCF8/0xCFC)
 *
 * Reicht fuer Bus-Scan, Klassencodes und BARs. Die Firmware hat die BARs bereits zugewiesen.
 * (Erweiterte Konfiguration ueber MCFG/ECAM ist spaeter noetig, z.B. fuer MSI-X.)
 */
#include "kernel.h"

#define MAX_PCI 64

static PciDev devs[MAX_PCI];
static int ndev;

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

void pci_init(void) {
    ndev = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            int nfn = ((hdr >> 16) & 0x80) ? 8 : 1;   /* Multifunktionsgeraet? */
            for (int fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                if (ndev >= MAX_PCI) return;
                uint32_t cls = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                PciDev *d = &devs[ndev++];
                d->bus = (uint8_t)bus;
                d->dev = (uint8_t)dev;
                d->fn = (uint8_t)fn;
                d->vendor = (uint16_t)(id & 0xFFFF);
                d->device = (uint16_t)(id >> 16);
                d->cls = (uint8_t)(cls >> 24);
                d->sub = (uint8_t)(cls >> 16);
                d->prog = (uint8_t)(cls >> 8);
            }
        }
    }
}

int pci_count(void) { return ndev; }

const PciDev *pci_get(int i) { return (i >= 0 && i < ndev) ? &devs[i] : 0; }

/* index-tes Geraet mit Klasse/Unterklasse/Prog-IF (prog 0xFF = egal), sonst 0 */
const PciDev *pci_find_class(uint8_t cls, uint8_t sub, uint8_t prog, int index) {
    for (int i = 0; i < ndev; i++) {
        if (devs[i].cls != cls || devs[i].sub != sub) continue;
        if (prog != 0xFF && devs[i].prog != prog) continue;
        if (index-- == 0) return &devs[i];
    }
    return 0;
}

/* Speicher-Zugriff und Bus-Master einschalten, INTx abschalten (wir pollen) */
void pci_enable(const PciDev *d) {
    uint32_t cmd = pci_read32(d->bus, d->dev, d->fn, 0x04) & 0xFFFF;   /* Status-Bits nicht zuruecksetzen */
    cmd |= 0x2 | 0x4 | 0x400;
    pci_write32(d->bus, d->dev, d->fn, 0x04, cmd);
}

/* Basisadresse (und Groesse) eines Speicher-BARs; 0 bei I/O-BAR oder nicht belegt */
uint64_t pci_bar(const PciDev *d, int n, uint64_t *size_out) {
    uint8_t off = (uint8_t)(0x10 + 4 * n);
    uint32_t bar = pci_read32(d->bus, d->dev, d->fn, off);
    if (size_out) *size_out = 0;
    if (bar & 1) return 0;   /* I/O-BAR */
    int is64 = ((bar >> 1) & 3) == 2;
    uint64_t base = bar & ~0xFULL;
    uint32_t hi = 0;
    if (is64) {
        hi = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(off + 4));
        base |= (uint64_t)hi << 32;
    }

    /* Groesse bestimmen: Decodierung aus, 1en schreiben, Maske lesen, alten Wert zurueck */
    uint32_t cmd = pci_read32(d->bus, d->dev, d->fn, 0x04) & 0xFFFF;
    pci_write32(d->bus, d->dev, d->fn, 0x04, cmd & ~0x7u);
    pci_write32(d->bus, d->dev, d->fn, off, 0xFFFFFFFFu);
    uint32_t lo = pci_read32(d->bus, d->dev, d->fn, off);
    pci_write32(d->bus, d->dev, d->fn, off, bar);
    uint64_t sz;
    if (is64) {
        pci_write32(d->bus, d->dev, d->fn, (uint8_t)(off + 4), 0xFFFFFFFFu);
        uint32_t h2 = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(off + 4));
        pci_write32(d->bus, d->dev, d->fn, (uint8_t)(off + 4), hi);
        uint64_t mask = ((uint64_t)h2 << 32) | (lo & ~0xFULL);
        sz = ~mask + 1;
    } else {
        sz = (uint32_t)(~(lo & ~0xFu) + 1);
    }
    pci_write32(d->bus, d->dev, d->fn, 0x04, cmd);
    if (size_out) *size_out = sz;
    return base;
}

const char *pci_class_name(uint8_t cls, uint8_t sub, uint8_t prog) {
    switch ((cls << 8) | sub) {
    case 0x0000: return "UNKLASSIFIZIERT";
    case 0x0100: return "SCSI";
    case 0x0101: return "IDE";
    case 0x0106: return prog == 1 ? "SATA (AHCI)" : "SATA";
    case 0x0108: return "NVME";
    case 0x0200: return "ETHERNET";
    case 0x0280: return "NETZWERK";
    case 0x0300: return "VGA";
    case 0x0302: return "3D-GRAFIK";
    case 0x0400: return "VIDEO";
    case 0x0403: return "AUDIO (HDA)";
    case 0x0600: return "HOST-BRIDGE";
    case 0x0601: return "ISA-BRIDGE";
    case 0x0604: return "PCI-BRIDGE";
    case 0x0C03: return prog == 0x30 ? "USB (XHCI)" : prog == 0x20 ? "USB (EHCI)" : "USB";
    case 0x0C05: return "SMBUS";
    default: return "ANDERES";
    }
}
