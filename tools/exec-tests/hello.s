# PLSE: gibt einen Text aus. kapi.puts liegt bei Offset 16.
.intel_syntax noprefix
push rbx
mov  rbx, rdi
lea  rdi, [rip+msg]
call [rbx+16]
pop  rbx
xor  eax, eax
ret
msg: .asciz "HALLO VON PLSE\n"
