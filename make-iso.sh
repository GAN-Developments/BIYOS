#!/bin/sh
# SPDX-License-Identifier: MIT
# Baut biyos.iso: bootbar per UEFI (El Torito) von CD/DVD, und - weil eine EFI-Systempartition angehaengt wird - auch
# nach dem Schreiben auf einen USB-Stick (dd). Beim Booten startet der Installer (install=1 in der boot.cfg der ISO).
#
# Voraussetzungen: xorriso, mtools, dosfstools (Ubuntu/WSL: sudo apt install xorriso mtools dosfstools)
# Vorher bauen:    make -C boot esp && make -C kernel esp && make -C modules esp
# Aufruf:          tools/make-iso.sh [ausgabe.iso]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-biyos.iso}
for tool in xorriso mkfs.vfat mcopy mmd; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Fehlt: $tool (sudo apt install xorriso mtools dosfstools)"; exit 1; }
done
ESP="$HERE/boot/esp"
[ -f "$ESP/BIYOS/kernel.elf" ] || { echo "Zuerst bauen: make -C boot esp && make -C kernel esp && make -C modules esp"; exit 1; }
BOOTEFI="$HERE/boot/BOOTX64.EFI"; [ -f "$BOOTEFI" ] || BOOTEFI="$ESP/EFI/BOOT/BOOTX64.EFI"
[ -f "$BOOTEFI" ] || { echo "BOOTX64.EFI fehlt (make -C boot esp)"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export MTOOLS_SKIP_CHECK=1

# boot.cfg der ISO: Installer starten, Module laden, alle Dateien als Nutzlast fuer den Installer bereitstellen
CFG="$TMP/boot.cfg"
{
    echo "# BIYOS Installations-Medium"
    echo "kernel=/BIYOS/kernel.elf"
    echo "install=1"
    echo "lang=de"
    echo "layout=de"
    echo
    for m in "$ESP"/BIYOS/modules/*.drv; do echo "module=/BIYOS/modules/$(basename "$m")"; done
    echo
    echo "payload=/EFI/BOOT/BOOTX64.EFI"
    echo "payload=/BIYOS/kernel.elf"
    for m in "$ESP"/BIYOS/modules/*.drv; do echo "payload=/BIYOS/modules/$(basename "$m")"; done
} > "$CFG"

# EFI-Systempartition als FAT-Abbild (16 MiB reichen)
IMG="$TMP/esp.img"
mkfs.vfat -F 16 -C "$IMG" 16384 >/dev/null
mmd -i "$IMG" ::EFI ::EFI/BOOT ::BIYOS ::BIYOS/modules
mcopy -i "$IMG" "$BOOTEFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$ESP/BIYOS/kernel.elf" ::BIYOS/kernel.elf
mcopy -i "$IMG" "$CFG" ::BIYOS/boot.cfg
for m in "$ESP"/BIYOS/modules/*.drv; do mcopy -i "$IMG" "$m" ::BIYOS/modules/; done

ROOT="$TMP/root"
mkdir -p "$ROOT"
cat > "$ROOT/README.TXT" <<'TXT'
BIYOS installation medium. Boot it in UEFI mode; the installer starts automatically.
Installationsmedium fuer BIYOS: im UEFI-Modus starten, der Installer startet von selbst.
TXT

xorriso -as mkisofs -quiet -iso-level 3 -r -J -joliet-long -V BIYOS \
    -append_partition 2 0xef "$IMG" \
    -e --interval:appended_partition_2:all:: -no-emul-boot \
    -isohybrid-gpt-basdat \
    -o "$OUT" "$ROOT"
echo "fertig: $OUT ($(wc -c < "$OUT") Bytes)"
