/*
 * mm.c - Speicherverwaltung des BIYOS-Kernels
 *
 *   1. PMM    : Bitmap-Allokator fuer physische 4-KiB-Seiten (aus der UEFI-Speicherkarte)
 *   2. Heap   : einfacher First-Fit-Allokator mit Zusammenfuehren freier Bloecke
 *   3. Paging : paging_map / paging_unmap auf der aktuellen CR3-Wurzel
 *
 * Alle diese Funktionen stehen in der Tabelle 'kapi' (kapi.h) und sind dadurch austauschbar.
 * Innerhalb von mm.c wird deshalb ebenfalls ueber kapi aufgerufen, wo ein Dienst ersetzbar
 * sein soll (z.B. holt der Heap neue Seiten ueber kapi.page_alloc).
 *
 * Bewusst ohne Schutz: Es wird nicht geprueft, wem ein Speicher gehoert.
 */
#include "kernel.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#define PAGE 4096ULL

/* ================================================================== */
/* PMM                                                                 */
/* ================================================================== */
#define PMM_MAX_PAGES (1ULL << 26)   /* verwaltet bis zu 256 GiB */

static uint64_t *bitmap;     /* 1 = belegt, 0 = frei */
static uint64_t pmm_limit;   /* Anzahl verwaltete Seiten */
static uint64_t pmm_free_cnt;
static uint64_t pmm_usable;  /* beim Start freigegebene Seiten */
static uint64_t pmm_hint;    /* hier beginnt die naechste Suche */

static inline int bm_test(uint64_t i) { return (int)((bitmap[i >> 6] >> (i & 63)) & 1); }
static inline void bm_set(uint64_t i) { bitmap[i >> 6] |= 1ULL << (i & 63); }
static inline void bm_clear(uint64_t i) { bitmap[i >> 6] &= ~(1ULL << (i & 63)); }

static const uint8_t *desc_at(const BootInfo *bi, uint64_t off) {
    return (const uint8_t *)(uintptr_t)(bi->mmap + off);
}

void mm_init(const BootInfo *bi) {
    /* hoechste RAM-Adresse bestimmen (EFI-Typen 1-7) */
    uint64_t top = 0;
    for (uint64_t off = 0; off + bi->mmap_desc_size <= bi->mmap_size; off += bi->mmap_desc_size) {
        const uint8_t *d = desc_at(bi, off);
        uint32_t t = *(const uint32_t *)d;
        if (t < 1 || t > 7) continue;
        uint64_t end = *(const uint64_t *)(d + 8) + *(const uint64_t *)(d + 24) * PAGE;
        if (end > top) top = end;
    }
    uint64_t npages = top / PAGE;
    if (npages > PMM_MAX_PAGES) npages = PMM_MAX_PAGES;
    uint64_t words = (npages + 63) / 64;
    uint64_t bpages = (words * 8 + PAGE - 1) / PAGE;

    /* Platz fuer die Bitmap in freiem Speicher ab 1 MiB suchen */
    uint64_t place = 0;
    for (uint64_t off = 0; off + bi->mmap_desc_size <= bi->mmap_size; off += bi->mmap_desc_size) {
        const uint8_t *d = desc_at(bi, off);
        if (*(const uint32_t *)d != 7) continue;
        uint64_t s = *(const uint64_t *)(d + 8);
        uint64_t e = s + *(const uint64_t *)(d + 24) * PAGE;
        if (s < 0x100000) s = 0x100000;
        if (e > s && (e - s) / PAGE >= bpages) {
            place = s;
            break;
        }
    }
    if (!place) {
        con_puts("PMM: KEIN PLATZ FUER BITMAP\n");
        kernel_halt();
    }
    bitmap = (uint64_t *)(uintptr_t)place;
    memset(bitmap, 0xFF, words * 8);   /* alles belegt */
    pmm_limit = npages;

    /* frei: freier Speicher (7) und Boot-Services-Code/-Daten (3, 4), alles ab 1 MiB.
     * Loader-Code/-Daten (1, 2) bleiben belegt: dort liegen Kernel, BootInfo, Module, Speicherkarte. */
    uint64_t freed = 0;
    for (uint64_t off = 0; off + bi->mmap_desc_size <= bi->mmap_size; off += bi->mmap_desc_size) {
        const uint8_t *d = desc_at(bi, off);
        uint32_t t = *(const uint32_t *)d;
        if (t != 3 && t != 4 && t != 7) continue;
        uint64_t s = *(const uint64_t *)(d + 8);
        uint64_t n = *(const uint64_t *)(d + 24);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t a = s + i * PAGE;
            uint64_t idx = a / PAGE;
            if (a < 0x100000) continue;
            if (idx >= npages) break;
            if (bm_test(idx)) {
                bm_clear(idx);
                freed++;
            }
        }
    }
    /* die Bitmap selbst belegen */
    for (uint64_t i = 0; i < bpages; i++) {
        uint64_t idx = place / PAGE + i;
        if (idx < npages && !bm_test(idx)) {
            bm_set(idx);
            freed--;
        }
    }
    pmm_free_cnt = freed;
    pmm_usable = freed;
    pmm_hint = 0;
}

uint64_t pmm_total_pages(void) { return pmm_usable; }
uint64_t pmm_free_page_count(void) { return pmm_free_cnt; }

static uint64_t pmm_alloc_pages_nl(uint64_t count) {
    if (!count || count > pmm_free_cnt) return 0;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t i = pass == 0 ? pmm_hint : 0;
        uint64_t end = pmm_limit;
        if (pass == 1) {
            end = pmm_hint + count;   /* Folgen, die ueber die Hinweisstelle reichen */
            if (end > pmm_limit) end = pmm_limit;
        }
        uint64_t run = 0, start = 0;
        for (; i < end; i++) {
            if ((i & 63) == 0 && bitmap[i >> 6] == ~0ULL) {   /* ganzes Wort belegt: ueberspringen */
                run = 0;
                i += 63;
                continue;
            }
            if (bm_test(i)) {
                run = 0;
                continue;
            }
            if (run == 0) start = i;
            if (++run == count) {
                for (uint64_t k = start; k < start + count; k++) bm_set(k);
                pmm_free_cnt -= count;
                pmm_hint = start + count;
                return start * PAGE;
            }
        }
    }
    return 0;
}

static void pmm_free_pages_nl(uint64_t addr, uint64_t count) {
    uint64_t first = addr / PAGE;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t idx = first + i;
        if (idx >= pmm_limit || !bm_test(idx)) continue;   /* ungueltig oder bereits frei: ignorieren */
        bm_clear(idx);
        pmm_free_cnt++;
    }
    if (first < pmm_hint) pmm_hint = first;
}

/* ================================================================== */
/* Heap                                                                */
/* ================================================================== */
#define BLOCK_MAGIC 0xB10C4B1D5EEDBEEFULL

typedef struct Block {
    uint64_t magic;
    size_t size;                 /* Nutzdaten ohne Kopf */
    struct Block *prev, *next;
    uint32_t free;
    uint32_t pad;
} __attribute__((aligned(16))) Block;

static Block *heap_head, *heap_tail;

static int heap_grow(size_t need) {
    uint64_t pages = ((uint64_t)need + sizeof(Block) + PAGE - 1) / PAGE;
    if (pages < 64) pages = 64;   /* mindestens 256 KiB am Stueck */
    uint64_t a = kapi.page_alloc(pages);
    if (!a) return 0;
    Block *b = (Block *)(uintptr_t)a;
    b->magic = BLOCK_MAGIC;
    b->size = pages * PAGE - sizeof(Block);
    b->prev = heap_tail;
    b->next = 0;
    b->free = 1;
    if (heap_tail) heap_tail->next = b;
    else heap_head = b;
    heap_tail = b;
    return 1;
}

static void *heap_malloc_nl(size_t n) {
    if (!n) n = 1;
    n = (n + 15) & ~(size_t)15;
    for (int attempt = 0; attempt < 2; attempt++) {
        for (Block *b = heap_head; b; b = b->next) {
            if (!b->free || b->size < n) continue;
            if (b->size >= n + sizeof(Block) + 16) {   /* Rest abspalten */
                Block *r = (Block *)((uint8_t *)(b + 1) + n);
                r->magic = BLOCK_MAGIC;
                r->size = b->size - n - sizeof(Block);
                r->free = 1;
                r->prev = b;
                r->next = b->next;
                if (b->next) b->next->prev = r;
                else heap_tail = r;
                b->next = r;
                b->size = n;
            }
            b->free = 0;
            return b + 1;
        }
        if (!heap_grow(n)) return 0;
    }
    return 0;
}

static void heap_free_nl(void *p) {
    if (!p) return;
    Block *b = (Block *)p - 1;
    if (b->magic != BLOCK_MAGIC || b->free) {
        con_puts("HEAP: UNGUELTIGES FREE\n");
        return;
    }
    b->free = 1;

    Block *n = b->next;   /* mit dem Nachfolger verschmelzen */
    if (n && n->free && (uint8_t *)(b + 1) + b->size == (uint8_t *)n) {
        b->size += sizeof(Block) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        else heap_tail = b;
        n->magic = 0;
    }
    Block *pv = b->prev;  /* mit dem Vorgaenger verschmelzen */
    if (pv && pv->free && (uint8_t *)(pv + 1) + pv->size == (uint8_t *)b) {
        pv->size += sizeof(Block) + b->size;
        pv->next = b->next;
        if (b->next) b->next->prev = pv;
        else heap_tail = pv;
        b->magic = 0;
    }
}

static void *heap_realloc_nl(void *p, size_t n) {
    if (!p) return kapi.malloc(n);
    Block *b = (Block *)p - 1;
    if (b->magic != BLOCK_MAGIC || b->free) return 0;
    if (n <= b->size) return p;
    void *q = kapi.malloc(n);
    if (!q) return 0;
    memcpy(q, p, b->size);
    kapi.free(p);
    return q;
}

/* ================================================================== */
/* Paging                                                              */
/* ================================================================== */
#define P_PRESENT 1ULL
#define P_RW      2ULL
#define P_PS      0x80ULL
#define ADDR_MASK 0x000FFFFFFFFFF000ULL

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void invlpg(uint64_t v) {
    __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
}

uint64_t *paging_root(void) {
    return (uint64_t *)(uintptr_t)(read_cr3() & ADDR_MASK);
}

static uint64_t *table_alloc(void) {
    uint64_t a = kapi.page_alloc(1);
    if (!a) return 0;
    memset((void *)(uintptr_t)a, 0, PAGE);
    return (uint64_t *)(uintptr_t)a;
}

/* Tabelle hinter einem Eintrag. Bei create wird sie angelegt. *err: -2 kein Speicher,
 * -3 grosse Seite (1 GiB) im Weg. Ohne create und ohne Eintrag: NULL mit *err == 0. */
static uint64_t *next_level(uint64_t *e, int create, int *err) {
    if (*e & P_PRESENT) {
        if (*e & P_PS) {
            *err = -3;
            return 0;
        }
        return (uint64_t *)(uintptr_t)(*e & ADDR_MASK);
    }
    if (!create) return 0;
    uint64_t *t = table_alloc();
    if (!t) {
        *err = -2;
        return 0;
    }
    *e = (uint64_t)(uintptr_t)t | P_PRESENT | P_RW;
    return t;
}

/* Seitentabelle (4 KiB) fuer eine virtuelle Adresse; teilt dabei eine 2-MiB-Seite auf. */
static uint64_t *pt_for(uint64_t virt, int create, int *err) {
    uint64_t *pml4 = paging_root();
    uint64_t *pdpt = next_level(&pml4[(virt >> 39) & 511], create, err);
    if (!pdpt) return 0;
    uint64_t *pd = next_level(&pdpt[(virt >> 30) & 511], create, err);
    if (!pd) return 0;
    uint64_t *pde = &pd[(virt >> 21) & 511];
    if ((*pde & P_PRESENT) && (*pde & P_PS)) {
        uint64_t old = *pde;
        uint64_t *pt = table_alloc();
        if (!pt) {
            *err = -2;
            return 0;
        }
        uint64_t base = old & 0x000FFFFFFFE00000ULL;
        uint64_t fl = (old & 0xFFFULL & ~P_PS)             /* Rechte und Cache-Bits uebernehmen */
                      | (((old >> 12) & 1) ? 0x80ULL : 0)  /* PAT-Bit sitzt bei 4 KiB auf Bit 7 */
                      | (old & (1ULL << 63));              /* NX */
        for (uint64_t i = 0; i < 512; i++) pt[i] = (base + i * PAGE) | fl;
        *pde = (uint64_t)(uintptr_t)pt | P_PRESENT | P_RW;
        invlpg(virt);
    }
    return next_level(pde, create, err);
}

static int map_page(uint64_t v, uint64_t p, uint64_t flags) {
    int err = 0;
    uint64_t *pt = pt_for(v, 1, &err);
    if (!pt) return err ? err : -2;
    pt[(v >> 12) & 511] = (p & ADDR_MASK) | flags | P_PRESENT;
    invlpg(v);
    return 0;
}

/* 0 = ok, -1 = nicht auf 4 KiB ausgerichtet, -2 = kein Speicher, -3 = 1-GiB-Seite im Weg */
static int paging_map_nl(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    if ((virt | phys | size) & (PAGE - 1)) return -1;
    for (uint64_t off = 0; off < size; off += PAGE) {
        int r = map_page(virt + off, phys + off, flags);
        if (r) return r;
    }
    return 0;
}

static int paging_unmap_nl(uint64_t virt, uint64_t size) {
    if ((virt | size) & (PAGE - 1)) return -1;
    for (uint64_t off = 0; off < size; off += PAGE) {
        uint64_t v = virt + off;
        int err = 0;
        uint64_t *pt = pt_for(v, 0, &err);
        if (!pt) {
            if (err) return err;
            continue;   /* war ohnehin nicht gemappt */
        }
        pt[(v >> 12) & 511] = 0;
        invlpg(v);
    }
    return 0;
}

/* ================================================================== */
/* Oeffentliche Funktionen: kurze kritische Abschnitte (Interrupts aus) */
/* ================================================================== */
/* Es gibt nur eine CPU. Damit der praeemptive Scheduler nicht mitten in einer dieser
 * Funktionen eine andere Task laufen laesst, sperren sie Interrupts nur waehrend der
 * Arbeit; verschachtelte Aufrufe (z.B. Heap -> Seitenallokator) sind erlaubt. */

uint64_t pmm_alloc_pages(uint64_t count) {
    uint64_t f = irq_save();
    uint64_t r = pmm_alloc_pages_nl(count);
    irq_restore(f);
    return r;
}

void pmm_free_pages(uint64_t addr, uint64_t count) {
    uint64_t f = irq_save();
    pmm_free_pages_nl(addr, count);
    irq_restore(f);
}

void *heap_malloc(size_t n) {
    uint64_t f = irq_save();
    void *r = heap_malloc_nl(n);
    irq_restore(f);
    return r;
}

void heap_free(void *p) {
    uint64_t f = irq_save();
    heap_free_nl(p);
    irq_restore(f);
}

void *heap_realloc(void *p, size_t n) {
    uint64_t f = irq_save();
    void *r = heap_realloc_nl(p, n);
    irq_restore(f);
    return r;
}

int paging_map(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    uint64_t f = irq_save();
    int r = paging_map_nl(virt, phys, size, flags);
    irq_restore(f);
    return r;
}

int paging_unmap(uint64_t virt, uint64_t size) {
    uint64_t f = irq_save();
    int r = paging_unmap_nl(virt, size);
    irq_restore(f);
    return r;
}
