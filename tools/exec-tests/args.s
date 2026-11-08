# PLSE: gibt argc und argv[1] aus und liefert argc als Rueckgabewert (kapi.dec = 32, putc = 8, puts = 16)
.intel_syntax noprefix
push rbx
push r12
push r13
mov  rbx, rdi
mov  r12d, esi
mov  r13, rdx
movsxd rdi, r12d
call [rbx+32]
mov  edi, 32
call [rbx+8]
cmp  r12d, 2
jl   1f
mov  rdi, [r13+8]
call [rbx+16]
1:
mov  edi, 10
call [rbx+8]
mov  eax, r12d
pop  r13
pop  r12
pop  rbx
ret
