#!/bin/sh
# SPDX-License-Identifier: MIT
# Baut die Testprogramme dieses Ordners zu BIYOS-Executables (braucht as/objcopy, also binutils).
# Aufruf: tools/exec-tests/build.sh [ausgabeordner]     (Standard: ./out)
# Danach z.B. mit mcopy auf die FAT32-Partition eines Testabbilds kopieren:
#   MTOOLS_SKIP_CHECK=1 mcopy -i nvme.img@@1048576 out/* ::
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-out}
MK="python3 $HERE/../make-module.py"
mkdir -p "$OUT"
TMP=$(mktemp -d)
for f in hello args call bg hi fail busy; do
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
rm -rf "$TMP"
echo "fertig: $OUT"
