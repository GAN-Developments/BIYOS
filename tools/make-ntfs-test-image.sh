#!/bin/sh
# SPDX-License-Identifier: MIT
# Creates a 128 MiB GPT disk image like make-test-image.sh, but partition 2 is a REAL NTFS volume
# filled with test content: files of different sizes, directories, a hard link, a named data stream,
# a 600-file directory (forces the NTFS B+tree index) and a sparse file.
#
# Needs FUSE access for ntfs-3g:  sudo apt install gdisk dosfstools mtools ntfs-3g
# Usage: tools/make-ntfs-test-image.sh [nvme-ntfs.img]
set -e
IMG=${1:-nvme-ntfs.img}
HERE=$(cd "$(dirname "$0")" && pwd)
sh "$HERE/make-test-image.sh" "$IMG" >/dev/null 2>&1
TMP=$(mktemp -d)
truncate -s 64M "$TMP/ntfs.img"
mkntfs -F -Q -L TESTVOL -s 512 -c 4096 "$TMP/ntfs.img" >/dev/null 2>&1
mkdir "$TMP/mnt"
ntfs-3g -o rw "$TMP/ntfs.img" "$TMP/mnt"
M="$TMP/mnt"
echo "Hallo von NTFS" > "$M/hallo.txt"
echo "Gemischte Schreibweise" > "$M/ReadMe.TXT"
echo "langer Name" > "$M/Eine sehr lange Datei mit Leerzeichen.txt"
head -c 307200 /dev/urandom > "$M/big.bin"
mkdir -p "$M/Docs/Sub"
echo "in Docs" > "$M/Docs/doc1.txt"
echo "tief unten" > "$M/Docs/Sub/deep.txt"
ln "$M/hallo.txt" "$M/hardlink.txt"
mkdir "$M/Many"
python3 - "$M" <<'PY'
import sys
m = sys.argv[1]
for i in range(1, 601):
    open(f'{m}/Many/lange_dateiname_fuer_index_{i:04d}.txt', 'w').write(f'datei {i}\n')
with open(f'{m}/sparse.bin', 'wb') as f:
    f.write(b'ANFANG'); f.seek(5 * 1024 * 1024); f.write(b'ENDE')
PY
umount "$M"
ntfs-3g -o rw,streams_interface=windows "$TMP/ntfs.img" "$M"   # second pass: named data stream
echo "Stromdaten" > "$M/hallo.txt:geheim"
umount "$M"
dd if="$TMP/ntfs.img" of="$IMG" bs=512 seek=67584 conv=notrunc 2>/dev/null   # partition 2 starts at LBA 67584
rm -rf "$TMP"
echo "done: $IMG   (boot: make -C boot run NVME_IMG=$(realpath "$IMG"))"
