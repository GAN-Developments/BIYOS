/*
 * BIYOS UEFI-Bootloader (x86_64)
 *
 * Ablauf:
 *   1. ESP oeffnen, \BIYOS\boot.cfg lesen (kernel=..., module=...)
 *   2. 3 Sekunden auf eine Taste warten -> Bootmenue (normal / ohne Module / einzeln waehlen)
 *   3. Kernel (ELF64) laden, Module (BIYOS-Executables) in den Speicher lesen
 *   4. Framebuffer (GOP) und ACPI-RSDP einsammeln
 *   5. Speicherkarte holen, ExitBootServices, Sprung in den Kernel mit BootInfo*
 */
#include "efi.h"
#include "bootinfo.h"

#define PAGE_SIZE   4096ULL
#define MAX_MODULES 32
#define MAX_PATH    128
#define CFG_PATH    "/BIYOS/boot.cfg"

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_HANDLE         IMG;
static EFI_FILE_PROTOCOL *g_root;

/* ------------------------------------------------------------------ */
/* Minimale libc-Ersatzfunktionen (GCC darf Aufrufe darauf erzeugen)   */
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

NOPAT void *memmove(void *d, const void *s, size_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    if (p < q) {
        while (n--) *p++ = *q++;
    } else {
        p += n;
        q += n;
        while (n--) *--p = *--q;
    }
    return d;
}

NOPAT int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++;
        q++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Konsolenausgabe und Eingabe                                         */
/* ------------------------------------------------------------------ */
static void putc_a(char c) {
    CHAR16 b[3];
    b[2] = 0;
    if (c == '\n') {
        b[0] = '\r';
        b[1] = '\n';
    } else {
        b[0] = (uint8_t)c;
        b[1] = 0;
    }
    ST->ConOut->OutputString(ST->ConOut, b);
}

static void puts_a(const char *s) {
    while (*s) putc_a(*s++);
}

static void put_hex(uint64_t v) {
    static const char h[] = "0123456789ABCDEF";
    char t[19];
    t[0] = '0';
    t[1] = 'x';
    for (int i = 0; i < 16; i++) t[2 + i] = h[(v >> (60 - 4 * i)) & 0xF];
    t[18] = 0;
    puts_a(t);
}

static void put_dec(uint64_t v) {
    char t[21];
    int i = 20;
    t[i] = 0;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    puts_a(&t[i]);
}

static CHAR16 wait_key(void) {
    EFI_INPUT_KEY k;
    UINTN idx;
    for (;;) {
        BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx);
        if (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) return k.UnicodeChar;
    }
}

/* wartet bis zu ms Millisekunden; 1 = Taste gedrueckt */
static int poll_key(int ms) {
    EFI_INPUT_KEY k;
    for (int i = 0; i < ms / 100; i++) {
        if (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) return 1;
        BS->Stall(100000);
    }
    return 0;
}

__attribute__((noreturn)) static void die(const char *msg, EFI_STATUS s) {
    puts_a("\nFEHLER: ");
    puts_a(msg);
    puts_a(" (Status ");
    put_hex(s);
    puts_a(")\nTaste druecken, um zur Firmware zurueckzukehren...\n");
    wait_key();
    BS->Exit(IMG, s ? s : EFI_LOAD_ERROR, 0, NULL);
    for (;;) {
    }
}

/* ------------------------------------------------------------------ */
/* Speicher und Dateien                                                */
/* ------------------------------------------------------------------ */
static UINTN pages_for(uint64_t bytes) {
    UINTN p = (UINTN)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    return p ? p : 1;
}

static void *alloc_pages(UINTN pages) {
    EFI_PHYSICAL_ADDRESS a = 0;
    EFI_STATUS s = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &a);
    if (EFI_ERROR(s)) die("AllocatePages fehlgeschlagen", s);
    memset((void *)(UINTN)a, 0, pages * PAGE_SIZE);
    return (void *)(UINTN)a;
}

static void to_wide(const char *s, CHAR16 *d, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = (s[i] == '/') ? (CHAR16)'\\' : (CHAR16)(uint8_t)s[i];
    d[i] = 0;
}

/* Liest eine komplette Datei von der ESP in frisch allozierten Speicher. */
static EFI_STATUS read_file(const char *path, void **buf, uint64_t *size) {
    CHAR16 wp[MAX_PATH + 4];
    EFI_FILE_PROTOCOL *f = NULL;
    to_wide(path, wp, MAX_PATH + 4);

    EFI_STATUS s = g_root->Open(g_root, &f, wp, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) return s;

    uint64_t sz = 0;
    f->SetPosition(f, ~0ULL);   /* ans Ende springen: Position = Dateigroesse */
    f->GetPosition(f, &sz);
    f->SetPosition(f, 0);

    EFI_PHYSICAL_ADDRESS a = 0;
    UINTN pages = pages_for(sz);
    s = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &a);
    if (EFI_ERROR(s)) {
        f->Close(f);
        return s;
    }
    memset((void *)(UINTN)a, 0, pages * PAGE_SIZE);

    UINTN rd = (UINTN)sz;
    s = f->Read(f, &rd, (void *)(UINTN)a);
    f->Close(f);
    if (EFI_ERROR(s) || rd != sz) {
        BS->FreePages(a, pages);
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    }
    *buf = (void *)(UINTN)a;
    *size = sz;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* boot.cfg                                                            */
/* ------------------------------------------------------------------ */
static char kernel_path[MAX_PATH] = "/BIYOS/kernel.elf";
static char mod_path[MAX_MODULES][MAX_PATH];
static uint8_t mod_enabled[MAX_MODULES];
static int mod_count;
static uint32_t boot_mode = BOOT_MODE_NORMAL;

static int starts(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static void scopy(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    while (i > 0 && (d[i - 1] == ' ' || d[i - 1] == '\t')) i--;   /* Leerzeichen am Ende weg */
    d[i] = 0;
}

/* Format: eine Zeile pro Eintrag, '#' beginnt einen Kommentar
 *   kernel=/BIYOS/kernel.elf
 *   module=/BIYOS/modules/video.drv                                */
static void parse_cfg(const char *t, uint64_t n) {
    uint64_t i = 0;
    while (i < n) {
        char line[MAX_PATH + 16];
        int l = 0;
        while (i < n && t[i] != '\n') {
            if (t[i] != '\r' && l < (int)sizeof(line) - 1) line[l++] = t[i];
            i++;
        }
        i++;
        line[l] = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == 0) continue;

        if (starts(p, "kernel=")) {
            scopy(kernel_path, p + 7, MAX_PATH);
        } else if (starts(p, "module=") && mod_count < MAX_MODULES) {
            scopy(mod_path[mod_count++], p + 7, MAX_PATH);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bootmenue                                                           */
/* ------------------------------------------------------------------ */
static void show_menu(void) {
    puts_a("\n  BIYOS Bootmenue\n");
    puts_a("  [1] Normal booten\n");
    puts_a("  [2] Ohne Module booten\n");
    puts_a("  [3] Module einzeln waehlen\n\n  > ");
    for (;;) {
        CHAR16 c = wait_key();
        if (c == '1') {
            boot_mode = BOOT_MODE_NORMAL;
            puts_a("1\n");
            break;
        }
        if (c == '2') {
            boot_mode = BOOT_MODE_NO_MODULES;
            puts_a("2\n");
            break;
        }
        if (c == '3') {
            boot_mode = BOOT_MODE_SELECTED;
            puts_a("3\n\n");
            for (int i = 0; i < mod_count; i++) {
                puts_a("  Laden: ");
                puts_a(mod_path[i]);
                puts_a(" (j/n) ");
                CHAR16 a;
                do {
                    a = wait_key();
                } while (a != 'j' && a != 'n' && a != 'J' && a != 'N');
                mod_enabled[i] = (a == 'j' || a == 'J');
                puts_a(mod_enabled[i] ? "j\n" : "n\n");
            }
            break;
        }
    }
    puts_a("\n");
}

/* ------------------------------------------------------------------ */
/* ELF64-Kernel laden                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Elf64_Phdr;

/* Laedt alle PT_LOAD-Segmente an ihre physische Adresse (p_paddr). */
static uint64_t load_kernel(void *img, uint64_t sz, uint64_t *lo, uint64_t *hi) {
    Elf64_Ehdr *eh = img;
    if (sz < sizeof(*eh) || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F')
        die("Kernel ist keine ELF-Datei", EFI_LOAD_ERROR);
    if (eh->ident[4] != 2 || eh->machine != 62)
        die("Kernel ist kein x86_64-ELF64", EFI_LOAD_ERROR);
    if (eh->phentsize < sizeof(Elf64_Phdr) || eh->phoff + (uint64_t)eh->phnum * eh->phentsize > sz)
        die("Kernel: Programmheader ungueltig", EFI_LOAD_ERROR);

    uint64_t mapped_end = 0;
    *lo = ~0ULL;
    *hi = 0;

    for (int i = 0; i < eh->phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)img + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != 1) continue;   /* nur PT_LOAD */
        if (ph->offset + ph->filesz > sz || ph->filesz > ph->memsz)
            die("Kernel: Segment ausserhalb der Datei", EFI_LOAD_ERROR);

        uint64_t start = ph->paddr & ~(PAGE_SIZE - 1);
        uint64_t end = (ph->paddr + ph->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t astart = (start < mapped_end) ? mapped_end : start;   /* Seiten des Vorgaengers ueberspringen */
        if (end > astart) {
            EFI_PHYSICAL_ADDRESS a = astart;
            EFI_STATUS s = BS->AllocatePages(AllocateAddress, EfiLoaderData, (UINTN)((end - astart) / PAGE_SIZE), &a);
            if (EFI_ERROR(s)) {
                puts_a("Zielbereich: ");
                put_hex(astart);
                puts_a(" - ");
                put_hex(end);
                puts_a("\n");
                die("Kernel-Zielspeicher nicht verfuegbar", s);
            }
            mapped_end = end;
        }
        memset((void *)(UINTN)ph->paddr, 0, (size_t)ph->memsz);
        memcpy((void *)(UINTN)ph->paddr, (uint8_t *)img + ph->offset, (size_t)ph->filesz);

        if (ph->paddr < *lo) *lo = ph->paddr;
        if (ph->paddr + ph->memsz > *hi) *hi = ph->paddr + ph->memsz;
    }
    if (*hi == 0) die("Kernel hat keine ladbaren Segmente", EFI_LOAD_ERROR);
    return eh->entry;
}

/* ------------------------------------------------------------------ */
/* BIYOS-Executable-Header pruefen: 0xAA "EXE" <4 Zeichen Typ> ...     */
/* ------------------------------------------------------------------ */
static int check_exe_header(const uint8_t *p, uint64_t sz) {
    return sz >= 20 && p[0] == 0xAA && p[1] == 'E' && p[2] == 'X' && p[3] == 'E';
}

static const char *basename_of(const char *p) {
    const char *b = p;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\') b = p + 1;
    }
    return b;
}

typedef void (__attribute__((sysv_abi)) *KernelEntry)(BootInfo *);

/* ------------------------------------------------------------------ */
/* Einstieg                                                            */
/* ------------------------------------------------------------------ */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    ST = st;
    BS = st->BootServices;
    IMG = image;

    BS->SetWatchdogTimer(0, 0, 0, NULL);
    ST->ConOut->ClearScreen(ST->ConOut);
    puts_a("BIYOS Bootloader\n================\n\n");

    /* --- ESP oeffnen --- */
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS s = BS->HandleProtocol(image, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li);
    if (EFI_ERROR(s)) die("LoadedImage-Protokoll fehlt", s);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    s = BS->HandleProtocol(li->DeviceHandle, &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&sfs);
    if (EFI_ERROR(s)) die("ESP nicht lesbar", s);
    s = sfs->OpenVolume(sfs, &g_root);
    if (EFI_ERROR(s)) die("ESP-Wurzelverzeichnis nicht oeffenbar", s);

    /* --- Konfiguration --- */
    void *cfg = NULL;
    uint64_t cfg_sz = 0;
    s = read_file(CFG_PATH, &cfg, &cfg_sz);
    if (EFI_ERROR(s))
        puts_a("Warnung: " CFG_PATH " nicht gefunden, nutze Standardwerte\n");
    else
        parse_cfg((const char *)cfg, cfg_sz);
    for (int i = 0; i < MAX_MODULES; i++) mod_enabled[i] = 1;

    /* --- Menue --- */
    puts_a("Taste druecken fuer Bootmenue (3 s)...\n");
    if (poll_key(3000)) show_menu();

    /* --- Kernel laden --- */
    void *kimg = NULL;
    uint64_t ksz = 0;
    s = read_file(kernel_path, &kimg, &ksz);
    if (EFI_ERROR(s)) {
        puts_a("Kernel-Pfad: ");
        puts_a(kernel_path);
        puts_a("\n");
        die("Kernel nicht gefunden", s);
    }
    uint64_t klo, khi;
    uint64_t entry = load_kernel(kimg, ksz, &klo, &khi);
    BS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)kimg, pages_for(ksz));
    puts_a("Kernel geladen: ");
    put_hex(klo);
    puts_a(" - ");
    put_hex(khi);
    puts_a(", Entry ");
    put_hex(entry);
    puts_a("\n");

    /* --- BootInfo + Modulliste --- */
    BootInfo *bi = alloc_pages(pages_for(sizeof(BootInfo) + MAX_MODULES * sizeof(BootModule)));
    BootModule *bm = (BootModule *)(bi + 1);
    bi->magic = BIYOS_BOOTINFO_MAGIC;
    bi->version = BIYOS_BOOTINFO_VERSION;
    bi->boot_mode = boot_mode;
    bi->kernel_base = klo;
    bi->kernel_end = khi;

    uint64_t nmod = 0;
    if (boot_mode != BOOT_MODE_NO_MODULES) {
        for (int i = 0; i < mod_count; i++) {
            if (!mod_enabled[i]) continue;
            void *mbuf = NULL;
            uint64_t msz = 0;
            s = read_file(mod_path[i], &mbuf, &msz);
            if (EFI_ERROR(s)) {
                puts_a("Warnung: Modul nicht gefunden: ");
                puts_a(mod_path[i]);
                puts_a("\n");
                continue;
            }
            if (!check_exe_header((const uint8_t *)mbuf, msz)) {
                puts_a("Warnung: kein gueltiger BIYOS-Header: ");
                puts_a(mod_path[i]);
                puts_a("\n");
                BS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)mbuf, pages_for(msz));
                continue;
            }
            scopy(bm[nmod].name, basename_of(mod_path[i]), (int)sizeof(bm[nmod].name));
            bm[nmod].addr = (uint64_t)(UINTN)mbuf;
            bm[nmod].size = msz;
            nmod++;
        }
    }
    bi->module_count = nmod;
    bi->modules = (uint64_t)(UINTN)bm;
    puts_a("Module geladen: ");
    put_dec(nmod);
    puts_a("\n");

    /* --- Framebuffer (aktueller GOP-Modus) --- */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (!EFI_ERROR(BS->LocateProtocol(&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, NULL, (void **)&gop)) && gop &&
        gop->Mode && gop->Mode->Info) {
        bi->fb_base = gop->Mode->FrameBufferBase;
        bi->fb_size = gop->Mode->FrameBufferSize;
        bi->fb_width = gop->Mode->Info->HorizontalResolution;
        bi->fb_height = gop->Mode->Info->VerticalResolution;
        bi->fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
        UINT32 pf = gop->Mode->Info->PixelFormat;
        bi->fb_format = (pf == 0) ? FB_FORMAT_RGBX : (pf == 1) ? FB_FORMAT_BGRX : FB_FORMAT_OTHER;
        puts_a("Framebuffer: ");
        put_dec(bi->fb_width);
        puts_a("x");
        put_dec(bi->fb_height);
        puts_a("\n");
    } else {
        puts_a("Warnung: kein Framebuffer (GOP) gefunden\n");
    }

    /* --- ACPI-RSDP --- */
    uint64_t rsdp = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
        if (!memcmp(&ct->VendorGuid, &EFI_ACPI_20_TABLE_GUID, sizeof(EFI_GUID))) {
            rsdp = (uint64_t)(UINTN)ct->VendorTable;
            break;
        }
        if (!rsdp && !memcmp(&ct->VendorGuid, &EFI_ACPI_10_TABLE_GUID, sizeof(EFI_GUID)))
            rsdp = (uint64_t)(UINTN)ct->VendorTable;
    }
    bi->rsdp = rsdp;

    /* --- Speicherkarte holen und Boot Services beenden ---
     * Ab dem ersten GetMemoryMap darf nichts mehr ausgegeben oder alloziert werden. */
    UINTN msize = 0, mkey = 0, dsize = 0;
    UINT32 dver = 0;
    puts_a("Starte Kernel...\n");
    s = BS->GetMemoryMap(&msize, NULL, &mkey, &dsize, &dver);
    if (s != EFI_BUFFER_TOO_SMALL || dsize == 0) die("GetMemoryMap: unerwartete Antwort", s);
    UINTN mpages = pages_for(msize + 16 * dsize);
    EFI_MEMORY_DESCRIPTOR *mbuf = alloc_pages(mpages);

    for (int tries = 0; tries < 8; tries++) {
        msize = mpages * PAGE_SIZE;
        s = BS->GetMemoryMap(&msize, mbuf, &mkey, &dsize, &dver);
        if (EFI_ERROR(s)) die("GetMemoryMap fehlgeschlagen", s);
        s = BS->ExitBootServices(image, mkey);
        if (!EFI_ERROR(s)) break;   /* Karte hat sich geaendert -> neu holen und erneut versuchen */
    }
    if (EFI_ERROR(s)) die("ExitBootServices fehlgeschlagen", s);

    /* --- ab hier keine UEFI-Dienste mehr --- */
    bi->mmap = (uint64_t)(UINTN)mbuf;
    bi->mmap_size = msize;
    bi->mmap_desc_size = dsize;
    bi->mmap_desc_version = dver;

    __asm__ volatile("cli");
    ((KernelEntry)(UINTN)entry)(bi);

    for (;;) __asm__ volatile("hlt");
    return EFI_SUCCESS;
}
