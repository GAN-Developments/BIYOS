/* Linkt ein C-Programm zu einem flachen, positionsunabhaengigen Binaer (der Einsprung muss zuerst kommen) */
ENTRY(entry)
SECTIONS {
    . = 0;
    .text   : { *(.text.entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) *(.bss .bss.* COMMON) }   /* .bss mit in die Datei, damit es 0 ist */
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
