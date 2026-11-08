#!/bin/sh
# SPDX-License-Identifier: MIT
# Erzeugt ein NVMe-Testabbild fuer QEMU: 128 MiB, GPT,
#   Partition 1 = FAT32 (mit HALLO.TXT), Partition 2 = Platzhalter mit NTFS-Kennung (zur Erkennung)
# Benoetigt: sudo apt install gdisk dosfstools mtools
# Benutzung: tools/make-test-image.sh [nvme.img]
set -e
IMG=${1:-nvme.img}
truncate -s 128M "$IMG"
sgdisk -o -n1:2048:+32M -t1:EF00 -c1:BIYOSESP -n2:0:+64M -t2:0700 -c2:DATEN "$IMG" >/dev/null
mkfs.vfat -F 32 --offset 2048 -n BIYOS "$IMG" $((32*1024)) >/dev/null
echo "Hallo von der FAT32-Partition" > /tmp/hallo.txt
MTOOLS_SKIP_CHECK=1 mcopy -i "$IMG@@1048576" /tmp/hallo.txt ::HALLO.TXT
python3 - "$IMG" <<'PY'
import sys
first = 67584   # Start-LBA von Partition 2 bei diesem Layout
with open(sys.argv[1], 'r+b') as f:
    f.seek(first * 512 + 3);   f.write(b'NTFS    ')
    f.seek(first * 512 + 510); f.write(b'\x55\xAA')
PY
echo "fertig: $IMG   (starten mit: cd boot && make run NVME_IMG=../$IMG)"
