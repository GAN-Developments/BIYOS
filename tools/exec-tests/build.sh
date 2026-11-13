#!/bin/sh
# SPDX-License-Identifier: MIT
# Baut die Testprogramme dieses Ordners zu BIYOS-Executables (braucht as/objcopy, also binutils).
# Aufruf: tools/exec-tests/build.sh [ausgabeordner]     (Standard: ./out)
# (C-Beispiele brauchen zusaetzlich gcc und ld.)
# Danach z.B. mit mcopy auf die FAT32-Partition eines Testabbilds kopieren:
#   MTOOLS_SKIP_CHECK=1 mcopy -i nvme.img@@1048576 out/* ::
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-out}
MK="python3 $HERE/../make-module.py"
mkdir -p "$OUT"
TMP=$(mktemp -d)
for f in hello args call bg hi fail busy start1 start2; do
    as -o "$TMP/$f.o" "$HERE/$f.s"
    objcopy -O binary -j .text "$TMP/$f.o" "$TMP/$f.bin"
done
# Offset des reservierten Platzes "slot" in call.s (fuer IMPORT)
SLOT=$(nm "$TMP/call.o" | awk '$3=="slot"{print $1}')
python3 -c "open('$OUT/ADD.BIN','wb').write(bytes([0x8d,0x04,0x37,0xc3]))"   # add: lea eax,[rdi+rsi]; ret
$MK "$OUT/hello.plse" PLSE 1 --bin "$TMP/hello.bin"
$MK "$OUT/args.plse"  PLSE 1 --bin "$TMP/args.bin"
$MK "$OUT/bg.plse"    PLSE 1 --bin "$TMP/bg.bin"
$MK "$OUT/call.plse"  PLSE 1 --bin "$TMP/call.bin" --import "ADD.BIN:0:4:+0x$SLOT"
$MK "$OUT/hi.drv"     DRIV 1 --bin "$TMP/hi.bin"
$MK "$OUT/fail.drv"   DRIV 1 --bin "$TMP/fail.bin"
$MK "$OUT/busy.drv"   DRIV 1 --bin "$TMP/busy.bin"
$MK "$OUT/start1.strt" STRT 1 --bin "$TMP/start1.bin"
$MK "$OUT/start2.strt" STRT 1 --bin "$TMP/start2.bin"
# C-Programm (hwtest.c): flach und positionsunabhaengig linken; der Einsprung "entry" steht in .text.entry
gcc -c -Os -ffreestanding -fPIE -fno-stack-protector -mno-red-zone -mgeneral-regs-only -fno-builtin \
    -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -fvisibility=hidden \
    -I"$HERE/../../kernel" -o "$TMP/hwtest.o" "$HERE/hwtest.c"
ld -T "$HERE/flat.ld" -o "$TMP/hwtest.elf" "$TMP/hwtest.o" 2>/dev/null
objcopy -O binary "$TMP/hwtest.elf" "$TMP/hwtest.bin"
$MK "$OUT/hwtest.drv" DRIV 1 --bin "$TMP/hwtest.bin"
# Ersatz-Shell: nach C:\Start kopieren (als mshell.strt), dann ersetzt sie beim Boot die eingebaute Shell
gcc -c -Os -ffreestanding -fPIE -fno-stack-protector -mno-red-zone -mgeneral-regs-only -fno-builtin \
    -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -fvisibility=hidden \
    -I"$HERE/../../kernel" -o "$TMP/mshell.o" "$HERE/mshell.c"
ld -T "$HERE/flat.ld" -o "$TMP/mshell.elf" "$TMP/mshell.o" 2>/dev/null
objcopy -O binary "$TMP/mshell.elf" "$TMP/mshell.bin"
$MK "$OUT/mshell.strt" STRT 1 --bin "$TMP/mshell.bin"
rm -rf "$TMP"
echo "fertig: $OUT"
