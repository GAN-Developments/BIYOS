# PLSE: ruft eine IMPORTierte Funktion (add: eax = edi + esi) ueber den reservierten Platz "slot" auf
.intel_syntax noprefix
push rbx
mov  rbx, rdi
mov  edi, 2
mov  esi, 3
call slot
mov  rdi, rax
call [rbx+32]
mov  edi, 10
call [rbx+8]
pop  rbx
xor  eax, eax
ret
slot: .fill 16, 1, 0x90
