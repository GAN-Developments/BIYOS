# STRT: wird beim Boot aus dem Start-Ordner gestartet
.intel_syntax noprefix
push rbx
mov  rbx, rdi
lea  rdi, [rip+msg]
call [rbx+16]
pop  rbx
xor  eax, eax
ret
msg: .asciz "STRT 2 LAEUFT\n"
