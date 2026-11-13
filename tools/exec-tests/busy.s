# DRIV, das das Entladen ablehnt (argc == -1 -> 1)
.intel_syntax noprefix
xor eax, eax
test esi, esi
jns 1f
mov eax, 1
1: ret
