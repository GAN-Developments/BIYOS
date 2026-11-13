# DRIV: meldet den Shell-Befehl HI an (kapi.cmd_register). Beim Entladen (argc == -1) nichts zu tun.
.intel_syntax noprefix
test esi, esi
js   unload
push rbx
mov  [rip+apislot], rdi
mov  rbx, rdi
lea  rdi, [rip+name]
lea  rsi, [rip+fn]
call [rbx+224]
pop  rbx
xor  eax, eax
ret
unload:
xor  eax, eax
ret
fn:
mov  rax, [rip+apislot]
lea  rdi, [rip+msg]
sub  rsp, 8
call [rax+16]
add  rsp, 8
xor  eax, eax
ret
apislot: .quad 0
name: .asciz "HI"
msg:  .asciz "HI VOM MODUL\n"
