/* DRIV in C: zeigt die Hardware-Funktionen von kapi v6.
 * Meldet die Befehle PCILIST, INJECT, RAMDISK und REMOUNT an. Bauen: siehe build.sh */
#include "kapi.h"

static KernelApi *g_api;
static BlockDev g_ram;
static unsigned char *g_ramdata;

static void put_hex_pair(unsigned a, unsigned b) {
    g_api->hex(a);
    g_api->putc(':');
    g_api->hex(b);
}

static int cmd_pcilist(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < g_api->pci_count(); i++) {
        const PciDev *d = g_api->pci_get(i);
        g_api->puts("PCI ");
        put_hex_pair(d->vendor, d->device);
        g_api->puts(" KLASSE ");
        put_hex_pair(d->cls, d->sub);
        g_api->putc('\n');
    }
    const PciDev *nvme = g_api->pci_find_class(0x01, 0x08, 0xFF, 0);   /* NVMe-Controller */
    if (nvme) {
        uint64_t size = 0, bar = g_api->pci_bar(nvme, 0, &size);
        g_api->puts("NVME BAR0 ");
        g_api->hex(bar);
        g_api->puts(" GROESSE ");
        g_api->hex(size);
        g_api->putc('\n');
    }
    return 0;
}

static int cmd_inject(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *s = "echo injiziert\n";   /* landet im Tastaturpuffer, die Shell fuehrt es aus */
    for (; *s; s++) g_api->kbd_inject(*s);
    return 0;
}

static int ram_read(BlockDev *d, uint64_t lba, uint32_t n, void *buf) {
    if (lba + n > d->sectors) return -3;
    volatile unsigned char *s = g_ramdata + lba * 512, *e = s + (uint64_t)n * 512;
    volatile unsigned char *o = buf;
    while (s < e) *o++ = *s++;
    return 0;
}

static int ram_write(BlockDev *d, uint64_t lba, uint32_t n, const void *buf) {
    if (lba + n > d->sectors) return -3;
    volatile unsigned char *o = g_ramdata + lba * 512;
    const volatile unsigned char *s = buf, *e = s + (uint64_t)n * 512;
    while (s < e) *o++ = *s++;
    return 0;
}

static int cmd_ramdisk(int argc, char **argv) {
    (void)argc; (void)argv;
    if (g_ram.name[0]) { g_api->puts("RAM0 SCHON DA\n"); return 0; }
    g_ramdata = g_api->malloc(128 * 512);
    if (!g_ramdata) return 1;
    for (int i = 0; i < 128 * 512; i++) g_ramdata[i] = 0;
    g_ram.name[0] = 'r'; g_ram.name[1] = 'a'; g_ram.name[2] = 'm'; g_ram.name[3] = '0';
    g_ram.sectors = 128;
    g_ram.sector_size = 512;
    g_ram.read = ram_read;      /* zur Laufzeit gesetzt: ein statischer Zeiger bräuchte eine Relokation */
    g_ram.write = ram_write;
    g_api->blk_register(&g_ram);
    g_api->blk_scan(&g_ram);
    g_api->puts("RAM0 ANGEMELDET\n");
    return 0;
}

static int cmd_remount(int argc, char **argv) {
    (void)argc; (void)argv;
    g_api->automount();   /* schon eingebundene Laufwerke bleiben unveraendert */
    return 0;
}

__attribute__((section(".text.entry"))) int entry(KernelApi *api, int argc, char **argv) {
    (void)argv;
    if (argc < 0) return 1;   /* nicht entladbar: RAM-Disk und Befehle bleiben */
    g_api = api;
    api->cmd_register("PCILIST", cmd_pcilist);
    api->cmd_register("INJECT", cmd_inject);
    api->cmd_register("RAMDISK", cmd_ramdisk);
    api->cmd_register("REMOUNT", cmd_remount);
    return 0;
}
