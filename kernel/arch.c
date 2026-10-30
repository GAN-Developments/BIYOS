/*
 * arch.c - CPU-Einrichtung des BIYOS-Kernels (x86_64)
 *
 * Designphilosophie: maximale Kontrolle fuer Programme, kein Schutz.
 *   - nur Ring 0, ein flaches Code-/Datensegment
 *   - Identity-Mapping des gesamten Adressraums, jede Seite present + schreibbar,
 *     das NX-Bit wird nirgends gesetzt (alles ausfuehrbar), CR0.WP wird geloescht
 *   - Exceptions werden trotzdem abgefangen und ausgegeben, damit ein Fehler kein stummer
 *     Triple-Fault-Reset ist (das schraenkt Programme nicht ein)
 */
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* CPU-Hilfsfunktionen                                                 */
/* ------------------------------------------------------------------ */
static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

/* ------------------------------------------------------------------ */
/* GDT + TSS                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t res0;
    uint64_t rsp[3];
    uint64_t res1;
    uint64_t ist[7];
    uint64_t res2;
    uint16_t res3;
    uint16_t iopb;
} __attribute__((packed)) Tss;

#define SEL_CODE 0x08
#define SEL_DATA 0x10
#define SEL_TSS  0x18

static uint64_t gdt[5] __attribute__((aligned(16)));
static Tss tss;
static uint8_t ist1_stack[16384] __attribute__((aligned(16)));   /* #DF */
static uint8_t ist2_stack[16384] __attribute__((aligned(16)));   /* NMI, #MC */

static void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 64-Bit-Code, Ring 0 */
    gdt[2] = 0x00CF92000000FFFFULL;   /* Daten, Ring 0 */

    tss.ist[0] = (uint64_t)(uintptr_t)(ist1_stack + sizeof(ist1_stack));
    tss.ist[1] = (uint64_t)(uintptr_t)(ist2_stack + sizeof(ist2_stack));
    tss.iopb = sizeof(Tss);

    uint64_t base = (uint64_t)(uintptr_t)&tss;
    uint64_t limit = sizeof(Tss) - 1;
    gdt[3] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[4] = base >> 32;

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr = {sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt};

    __asm__ volatile("lgdt %0" : : "m"(gdtr));
    __asm__ volatile(
        "pushq %[cs]\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov %[ds], %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        :
        : [cs] "i"(SEL_CODE), [ds] "i"(SEL_DATA)
        : "rax", "memory");
    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)SEL_TSS));
}

/* ------------------------------------------------------------------ */
/* IDT                                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed)) IdtEntry;

extern const uint64_t isr_table[256];   /* aus isr.S */

static IdtEntry idt[256] __attribute__((aligned(16)));
static IrqHandler handlers[256];

void idt_set_handler(uint8_t vector, IrqHandler fn) {
    handlers[vector] = fn;
}

static void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        uint64_t a = isr_table[i];
        idt[i].off_lo = (uint16_t)a;
        idt[i].sel = SEL_CODE;
        idt[i].ist = (i == 8) ? 1 : (i == 2 || i == 18) ? 2 : 0;
        idt[i].attr = 0x8E;   /* present, Ring 0, Interrupt-Gate */
        idt[i].off_mid = (uint16_t)(a >> 16);
        idt[i].off_hi = (uint32_t)(a >> 32);
        idt[i].zero = 0;
    }
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {sizeof(idt) - 1, (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */
static const char *const exc_names[32] = {
    "#DE DIVIDE ERROR",        "#DB DEBUG",               "NMI",                     "#BP BREAKPOINT",
    "#OF OVERFLOW",            "#BR BOUND RANGE",         "#UD INVALID OPCODE",      "#NM DEVICE NOT AVAILABLE",
    "#DF DOUBLE FAULT",        "COPROCESSOR OVERRUN",     "#TS INVALID TSS",         "#NP SEGMENT NOT PRESENT",
    "#SS STACK FAULT",         "#GP GENERAL PROTECTION",  "#PF PAGE FAULT",          "RESERVED",
    "#MF X87 FLOATING POINT",  "#AC ALIGNMENT CHECK",     "#MC MACHINE CHECK",       "#XM SIMD FLOATING POINT",
    "#VE VIRTUALIZATION",      "#CP CONTROL PROTECTION",  "RESERVED",                "RESERVED",
    "RESERVED",                "RESERVED",                "RESERVED",                "RESERVED",
    "#HV HYPERVISOR",          "#VC VMM COMMUNICATION",   "#SX SECURITY",            "RESERVED",
};

static void reg(const char *name, uint64_t v) {
    con_puts(name);
    con_puts("=");
    con_hex64(v);
    con_puts(" ");
}

static void exception_dump(const Regs *r) {
    con_color(255, 90, 90);
    con_puts("\nEXCEPTION ");
    con_dec(r->vector);
    con_puts(": ");
    con_puts(r->vector < 32 ? exc_names[r->vector] : "UNBEKANNT");
    con_puts("\n");

    con_color(230, 230, 230);
    if (r->vector == 14) {
        con_puts("PAGE FAULT: ");
        con_puts((r->error & 1) ? "PROTECTION" : "NOT PRESENT");
        con_puts((r->error & 2) ? ", WRITE" : ", READ");
        con_puts((r->error & 16) ? ", INSTRUCTION FETCH" : "");
        con_puts(" AT ");
        con_hex64(read_cr2());
        con_puts("\n");
    }
    reg("RIP", r->rip); reg("CS", r->cs); reg("RFLAGS", r->rflags); con_puts("\n");
    reg("RSP", r->rsp); reg("SS", r->ss); reg("ERR", r->error); con_puts("\n");
    reg("RAX", r->rax); reg("RBX", r->rbx); reg("RCX", r->rcx); con_puts("\n");
    reg("RDX", r->rdx); reg("RSI", r->rsi); reg("RDI", r->rdi); con_puts("\n");
    reg("RBP", r->rbp); reg("R8", r->r8);   reg("R9", r->r9);   con_puts("\n");
    reg("R10", r->r10); reg("R11", r->r11); reg("R12", r->r12); con_puts("\n");
    reg("R13", r->r13); reg("R14", r->r14); reg("R15", r->r15); con_puts("\n");
    con_color(255, 90, 90);
    con_puts("SYSTEM HALTED\n");
}

/* Breakpoint (int3) ist eine Falle: ausgeben und weiterlaufen */
static void breakpoint_handler(Regs *r) {
    con_puts("BREAKPOINT AT ");
    con_hex64(r->rip);
    con_puts("\n");
}

/* Wird von isr_common aufgerufen. Rueckgabe: der Registerrahmen, mit dem fortgesetzt wird.
 * Normalerweise derselbe (r), bei einem Taskwechsel der Rahmen der naechsten Task. */
Regs *interrupt_handler(Regs *r);
Regs *interrupt_handler(Regs *r) {
    if (r->vector < 256 && handlers[r->vector]) {
        handlers[r->vector](r);
    } else if (r->vector < 32) {
        exception_dump(r);
        kernel_halt();
    }
    /* nicht behandelte Interrupts >= 32 werden ignoriert */
    return sched_after_interrupt(r);
}

/* ------------------------------------------------------------------ */
/* Seitentabellen: Identity-Mapping, alles present + RW, kein NX        */
/* ------------------------------------------------------------------ */
#define MAX_GIB 64

#define PTE_P  0x01ULL
#define PTE_RW 0x02ULL
#define PTE_PS 0x80ULL
#define PTE_WC 0x08ULL   /* PWT      -> PAT-Index 1 = Write-Combining */
#define PTE_UC 0x18ULL   /* PWT|PCD  -> PAT-Index 3 = Uncacheable */

static uint64_t pml4[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pd[MAX_GIB][512] __attribute__((aligned(4096)));

/* UEFI-Speichertypen, die echter Arbeitsspeicher sind (cachebar) */
static int is_ram_type(uint32_t t) {
    return (t >= 1 && t <= 7) || t == 9 || t == 10 || t == 13 || t == 14;
}

static int overlaps_ram(const BootInfo *bi, uint64_t s, uint64_t e) {
    for (uint64_t off = 0; off + bi->mmap_desc_size <= bi->mmap_size; off += bi->mmap_desc_size) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)(bi->mmap + off);
        if (!is_ram_type(*(const uint32_t *)d)) continue;
        uint64_t ds = *(const uint64_t *)(d + 8);
        uint64_t de = ds + *(const uint64_t *)(d + 24) * 4096ULL;
        if (ds < e && de > s) return 1;
    }
    return 0;
}

static void paging_init(const BootInfo *bi) {
    /* hoechste RAM-Adresse (und Framebuffer-Ende) bestimmen */
    uint64_t top = 0;
    for (uint64_t off = 0; off + bi->mmap_desc_size <= bi->mmap_size; off += bi->mmap_desc_size) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)(bi->mmap + off);
        if (!is_ram_type(*(const uint32_t *)d)) continue;
        uint64_t de = *(const uint64_t *)(d + 8) + *(const uint64_t *)(d + 24) * 4096ULL;
        if (de > top) top = de;
    }
    uint64_t fb_s = bi->fb_base, fb_e = bi->fb_base ? bi->fb_base + bi->fb_size : 0;
    if (fb_e > top) top = fb_e;

    uint64_t gib = (top + (1ULL << 30) - 1) >> 30;
    if (gib < 4) gib = 4;   /* mindestens 4 GiB: deckt die Geraete-MMIO unter 4 GiB ab (APIC usw.) */
    if (gib > MAX_GIB) {
        con_puts("WARNUNG: NUR ERSTE 64 GIB GEMAPPT\n");
        gib = MAX_GIB;
    }

    uint64_t wb = 0, wc = 0, uc = 0;
    for (uint64_t g = 0; g < gib; g++) {
        pdpt[g] = (uint64_t)(uintptr_t)pd[g] | PTE_P | PTE_RW;
        for (uint64_t e = 0; e < 512; e++) {
            uint64_t a = (g << 30) + (e << 21);
            uint64_t attr;
            if (overlaps_ram(bi, a, a + (2ULL << 20))) {
                attr = 0;          /* Write-Back */
                wb++;
            } else if (fb_e && fb_s < a + (2ULL << 20) && fb_e > a) {
                attr = PTE_WC;     /* Framebuffer: Write-Combining */
                wc++;
            } else {
                attr = PTE_UC;     /* Geraete/Luecken: Uncacheable */
                uc++;
            }
            pd[g][e] = a | PTE_P | PTE_RW | PTE_PS | attr;   /* kein NX-Bit -> ausfuehrbar */
        }
    }
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_P | PTE_RW;

    write_cr0(read_cr0() & ~(1ULL << 16));   /* WP aus: Ring 0 darf alles schreiben */

    uint64_t cr4 = read_cr4();
    write_cr4(cr4 & ~(1ULL << 7));           /* PGE aus: globale Eintraege der Firmware verwerfen */
    write_cr3((uint64_t)(uintptr_t)pml4);

    /* PAT: Index 0 = WB, 1 = WC, 2 = UC-, 3 = UC (4-7 wiederholt) */
    wrmsr(0x277, 0x0007010600070106ULL);
    __asm__ volatile("wbinvd" : : : "memory");
    write_cr3((uint64_t)(uintptr_t)pml4);    /* TLB nach PAT-Aenderung leeren */
    write_cr4(cr4);

    con_puts("PAGING: ");
    con_dec(gib);
    con_puts(" GIB IDENTITY, ");
    con_dec(wb);
    con_puts(" WB / ");
    con_dec(wc);
    con_puts(" WC / ");
    con_dec(uc);
    con_puts(" UC (2 MIB SEITEN)\n");
}

/* ------------------------------------------------------------------ */
void arch_init(const BootInfo *bi) {
    gdt_init();
    con_puts("GDT + TSS OK\n");
    idt_init();
    idt_set_handler(3, breakpoint_handler);
    con_puts("IDT OK (256 VEKTOREN)\n");
    paging_init(bi);
}
