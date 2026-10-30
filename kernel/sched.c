/*
 * sched.c - Timer und Scheduler des BIYOS-Kernels
 *
 * Timer    : Local-APIC-Timer, gegen den PIT (Kanal 2) auf 10 ms kalibriert, Vektor 0x20.
 * Scheduler: praeemptives Round-Robin ueber eine zirkulaere Liste von Tasks, nur Ring 0.
 *
 * Funktionsweise: Der Zustand einer nicht laufenden Task ist ein Regs-Rahmen (siehe kapi.h) auf
 * ihrem eigenen Stack, genau wie ihn isr_common beim Interrupt aufbaut. Ein Taskwechsel
 * liefert deshalb nur den Rahmen der naechsten Task zurueck, und isr_common setzt den
 * Stackzeiger darauf und kehrt per iretq in diese Task zurueck.
 *
 * Freiwilliges Abgeben (yield) ist ein Software-Interrupt (int 0x82) und laeuft ueber denselben
 * Weg. Eine Task darf sich dem Scheduler entziehen, indem sie Interrupts sperrt (cli) - das ist
 * bewusst erlaubt. Kritische Abschnitte im Kernel (Heap, Seitenverwaltung, Konsole) sperren
 * kurz selbst. Es gibt nur eine CPU.
 *
 * Hinweis: Der FPU/SSE-Zustand wird nicht gesichert. Der Kernel nutzt kein SSE
 * (-mgeneral-regs-only). Programme, die SSE nutzen, brauchen spaeter fxsave pro Task.
 */
#include "kernel.h"

void *memset(void *d, int c, size_t n);

#define TICK_MS        10      /* ein Timer-Tick = 10 ms (100 Hz) */
#define TIMER_VECTOR   0x20
#define YIELD_VECTOR   0x82
#define SPURIOUS_VEC   0xFF
#define STACK_PAGES    16      /* 64 KiB Stack pro Task */

#define SEL_CODE 0x08
#define SEL_DATA 0x10

/* ------------------------------------------------------------------ */
/* Local APIC                                                          */
/* ------------------------------------------------------------------ */
#define LAPIC_SVR      0x0F0
#define LAPIC_TPR      0x080
#define LAPIC_EOI      0x0B0
#define LAPIC_LVT_TMR  0x320
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR  0x390
#define LAPIC_TMR_DIV  0x3E0

static int x2apic;
static volatile uint32_t *lapic;

static uint32_t lapic_read(uint32_t reg) {
    if (x2apic) return (uint32_t)rdmsr(0x800 + (reg >> 4));
    return lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t v) {
    if (x2apic) wrmsr(0x800 + (reg >> 4), v);
    else lapic[reg / 4] = v;
}

static void lapic_init(void) {
    uint64_t base = rdmsr(0x1B);
    if (!(base & (1ULL << 11))) {   /* APIC einschalten, falls aus */
        wrmsr(0x1B, base | (1ULL << 11));
        base = rdmsr(0x1B);
    }
    x2apic = (int)((base >> 10) & 1);
    if (!x2apic) lapic = (volatile uint32_t *)(uintptr_t)(base & 0xFFFFF000ULL);
    lapic_write(LAPIC_SVR, 0x100 | SPURIOUS_VEC);   /* APIC an, Spurious-Vektor 0xFF */
    lapic_write(LAPIC_TPR, 0);                       /* alle Prioritaeten zulassen */
}

/* LAPIC-Timer-Takte (Teiler 16) in 10 ms, gemessen am PIT (Kanal 2, 1193182 Hz) */
static uint32_t lapic_calibrate(void) {
    lapic_write(LAPIC_TMR_DIV, 0x3);        /* Teiler 16 */
    lapic_write(LAPIC_LVT_TMR, 0x10000);    /* maskiert, einmalig */

    outb(0x61, (uint8_t)((inb(0x61) & 0xFC) | 0x01));   /* Gate an, Lautsprecher aus */
    outb(0x43, 0xB0);                                    /* Kanal 2, lo/hi, Modus 0 */
    outb(0x42, 11932 & 0xFF);                            /* 11932 Takte = 10 ms */
    outb(0x42, 11932 >> 8);
    lapic_write(LAPIC_TMR_INIT, 0xFFFFFFFF);

    for (uint32_t i = 0; i < 50000000 && !(inb(0x61) & 0x20); i++) {
    }
    uint32_t cur = lapic_read(LAPIC_TMR_CUR);
    lapic_write(LAPIC_TMR_INIT, 0);
    if (inb(0x61) & 0x20) return 0xFFFFFFFFu - cur;
    return 0;   /* Zeitueberschreitung */
}

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */
enum { T_READY = 1, T_SLEEP = 2, T_DEAD = 3 };

typedef struct Task {
    uint64_t id;
    char name[24];
    Regs *frame;                  /* gespeicherter Zustand, solange die Task nicht laeuft */
    uint64_t stack_phys;          /* 0 bei der Haupt-Task (hat den Start-Stack) */
    uint64_t stack_pages;
    int state;
    uint64_t wake_tick;
    uint64_t run_ticks;
    struct Task *next, *prev;
} Task;

static Task main_task;
static Task *g_cur;
static uint64_t g_next_id = 1;
static volatile uint64_t g_ticks;
static volatile int g_need_resched;

uint64_t timer_uptime_ms(void) {
    return g_ticks * TICK_MS;
}

static void timer_irq(Regs *r) {
    (void)r;
    g_ticks++;
    lapic_write(LAPIC_EOI, 0);
    g_need_resched = 1;
}

static void yield_irq(Regs *r) {
    (void)r;
    g_need_resched = 1;
}

/* Wird am Ende jedes Interrupts aufgerufen (arch.c) und liefert den Rahmen zum Fortsetzen */
static Regs *sched_schedule(Regs *r) {
    Task *cur = g_cur;
    cur->frame = r;
    cur->run_ticks++;

    /* tote Tasks einsammeln (nie die laufende: wir stehen noch auf ihrem Stack) */
    for (Task *t = cur->next; t != cur;) {
        Task *n = t->next;
        if (t->state == T_DEAD) {
            t->prev->next = t->next;
            t->next->prev = t->prev;
            kapi.page_free(t->stack_phys, t->stack_pages);
            kapi.free(t);
        }
        t = n;
    }

    /* Schlafende aufwecken */
    Task *t = cur;
    do {
        if (t->state == T_SLEEP && g_ticks >= t->wake_tick) t->state = T_READY;
        t = t->next;
    } while (t != cur);

    /* naechste bereite Task nach der aktuellen */
    Task *n = cur->next;
    while (n != cur && n->state != T_READY) n = n->next;
    if (n == cur && cur->state != T_READY) n = &main_task;   /* Leerlauf: Haupt-Task */
    g_cur = n;
    return n->frame;
}

Regs *sched_after_interrupt(Regs *r) {
    if (!g_cur || !g_need_resched) return r;
    g_need_resched = 0;
    return sched_schedule(r);
}

void sched_yield(void) {
    __asm__ volatile("int $0x82" : : : "memory");
}

void task_exit(void) {
    uint64_t f = irq_save();
    g_cur->state = T_DEAD;
    irq_restore(f);
    for (;;) sched_yield();   /* kehrt nie zurueck: die Task wird beim naechsten Wechsel entfernt */
}

static void task_return_stub(void) {
    task_exit();   /* Ruecksprungadresse am Stack-Ende: Funktion ist zurueckgekehrt */
}

/* Startzustand einer neuen Task: laeuft beim ersten Wechsel per iretq los */
static Regs *make_frame(uint64_t stack_top, void (*entry)(void *), void *arg) {
    uint64_t *sp = (uint64_t *)(uintptr_t)(stack_top - 8);   /* (rsp % 16 == 8) wie nach einem call */
    *sp = (uint64_t)(uintptr_t)task_return_stub;
    Regs *fr = (Regs *)(uintptr_t)(stack_top - 512 - sizeof(Regs));
    memset(fr, 0, sizeof(Regs));
    fr->rip = (uint64_t)(uintptr_t)entry;
    fr->cs = SEL_CODE;
    fr->ss = SEL_DATA;
    fr->rflags = 0x202;   /* IF = 1 */
    fr->rsp = (uint64_t)(uintptr_t)sp;
    fr->rdi = (uint64_t)(uintptr_t)arg;
    return fr;
}

uint64_t task_create(const char *name, void (*entry)(void *), void *arg) {
    Task *t = kapi.malloc(sizeof(Task));
    if (!t) return 0;
    uint64_t sp = kapi.page_alloc(STACK_PAGES);
    if (!sp) {
        kapi.free(t);
        return 0;
    }
    memset(t, 0, sizeof(Task));
    for (int i = 0; i < 23 && name && name[i]; i++) t->name[i] = name[i];
    t->stack_phys = sp;
    t->stack_pages = STACK_PAGES;
    t->frame = make_frame(sp + STACK_PAGES * 4096ULL, entry, arg);
    t->state = T_READY;

    uint64_t f = irq_save();
    t->id = g_next_id++;
    t->next = g_cur->next;   /* direkt hinter der laufenden Task einreihen */
    t->prev = g_cur;
    g_cur->next->prev = t;
    g_cur->next = t;
    irq_restore(f);
    return t->id;
}

int task_kill(uint64_t id) {
    uint64_t f = irq_save();
    Task *t = g_cur;
    do {
        if (t->id == id) break;
        t = t->next;
    } while (t != g_cur);
    if (t->id != id || t == &main_task) {
        irq_restore(f);
        return -1;   /* unbekannt oder die Haupt-Task */
    }
    t->state = T_DEAD;
    int self = (t == g_cur);
    irq_restore(f);
    if (self) for (;;) sched_yield();
    return 0;
}

void sched_sleep_ms(uint64_t ms) {
    uint64_t wake = g_ticks + (ms + TICK_MS - 1) / TICK_MS;
    if (g_cur == &main_task) {   /* die Haupt-Task ist der Leerlauf und darf nicht schlafen */
        while (g_ticks < wake) __asm__ volatile("sti\n\thlt" : : : "memory");
        return;
    }
    uint64_t f = irq_save();
    g_cur->state = T_SLEEP;
    g_cur->wake_tick = wake;
    irq_restore(f);
    sched_yield();
}

/* ------------------------------------------------------------------ */
void sched_init(void) {
    /* altes PIC stilllegen, Local APIC einrichten */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    lapic_init();

    uint32_t per10ms = lapic_calibrate();
    if (!per10ms) {
        con_puts("TIMER: KALIBRIERUNG FEHLGESCHLAGEN, NUTZE SCHAETZWERT\n");
        per10ms = 62500;
    }

    main_task.id = g_next_id++;
    main_task.state = T_READY;
    main_task.name[0] = 'K'; main_task.name[1] = 'E'; main_task.name[2] = 'R';
    main_task.name[3] = 'N'; main_task.name[4] = 'E'; main_task.name[5] = 'L';
    main_task.next = main_task.prev = &main_task;
    g_cur = &main_task;

    idt_set_handler(TIMER_VECTOR, timer_irq);
    idt_set_handler(YIELD_VECTOR, yield_irq);

    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, TIMER_VECTOR | 0x20000);   /* periodisch */
    lapic_write(LAPIC_TMR_INIT, per10ms);

    con_puts("TIMER: ");
    con_puts(x2apic ? "X2APIC, " : "LAPIC, ");
    con_dec(1000 / TICK_MS);
    con_puts(" HZ (");
    con_dec(per10ms);
    con_puts(" TAKTE/10 MS)\n");

    __asm__ volatile("sti" : : : "memory");
}
