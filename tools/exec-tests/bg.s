# PLSE zum Starten im Hintergrund: schlaeft 300 ms und gibt dann etwas aus (sleep_ms = 152)
.intel_syntax noprefix
push rbx
mov  rbx, rdi
mov  edi, 300
call [rbx+152]
lea  rdi, [rip+msg]
call [rbx+16]
pop  rbx
xor  eax, eax
ret
msg: .asciz "HINTERGRUND FERTIG\n"
