# isr.S - Einsprungpunkte fuer alle 256 Interrupt-Vektoren
#
# Jeder Stub legt (falls die CPU keinen Fehlercode liefert) eine 0 als Fehlercode und danach
# die Vektornummer auf den Stack und springt nach isr_common. isr_common sichert alle
# Allzweckregister und ruft interrupt_handler(Regs *) in arch.c auf.

    .text
    .altmacro

    .macro ISR_NOERR n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
    .endm

    .macro ISR_ERR n
isr\n:
    pushq $\n
    jmp isr_common
    .endm

    # Vektoren mit Fehlercode: 8, 10-14, 17, 21, 29, 30
    .macro ISR_AUTO n
    .if (\n == 8) || ((\n >= 10) && (\n <= 14)) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
    ISR_ERR \n
    .else
    ISR_NOERR \n
    .endif
    .endm

    .set vec, 0
    .rept 256
    ISR_AUTO %vec
    .set vec, vec+1
    .endr

isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq %rsp, %rdi          # Regs *
    call interrupt_handler   # Stack ist hier 16-Byte-ausgerichtet (CPU + 2 + 15 Pushes)
    movq %rax, %rsp          # Rueckgabe = Rahmen zum Fortsetzen (bei Taskwechsel der einer anderen Task)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp           # Vektor + Fehlercode entfernen
    iretq

    .macro ISR_ADDR n
    .quad isr\n
    .endm

    .section .rodata
    .balign 8
    .global isr_table
isr_table:
    .set vec, 0
    .rept 256
    ISR_ADDR %vec
    .set vec, vec+1
    .endr

    .section .note.GNU-stack,"",@progbits
