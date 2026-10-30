/* BIYOS Test-Kernel: physisch = virtuell, ab 1 MiB.
 * Bei OVMF ist 0x100000-0x800000 freier Speicher (16 MiB lag dort in BootServicesData).
 * Jede Sektion beginnt auf einer eigenen 4-KiB-Seite, damit der Bootloader die
 * PT_LOAD-Segmente ohne Seitenueberlappung allozieren kann. */
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(4096) { *(.text*) }
    .rodata : ALIGN(4096) { *(.rodata*) }
    .data : ALIGN(4096) { *(.data*) }
    .bss : ALIGN(4096) { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
