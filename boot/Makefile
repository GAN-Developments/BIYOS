# BIYOS UEFI-Bootloader
#
# WSL (Ubuntu):  sudo apt install gcc-mingw-w64-x86-64 qemu-system-x86 ovmf make
# MSYS2:         MINGW64-Shell, pacman -S mingw-w64-x86_64-gcc make
#                (dort heisst der Compiler ebenfalls x86_64-w64-mingw32-gcc)
#
#   make        -> BOOTX64.EFI bauen
#   make esp    -> Ordner esp/ mit EFI-Struktur anlegen (FAT-Ersatz fuer QEMU)
#   make run    -> in QEMU mit OVMF starten (OVMF-Pfad ggf. anpassen)

CC      := x86_64-w64-mingw32-gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -ffreestanding -fno-stack-protector -fno-stack-check \
           -mno-red-zone -fno-asynchronous-unwind-tables
LDFLAGS := -nostdlib -Wl,-dll -shared -Wl,--subsystem,10 -e efi_main
OVMF    ?= /usr/share/ovmf/OVMF.fd

.PHONY: all esp run clean

all: BOOTX64.EFI

main.o: main.c efi.h bootinfo.h
	$(CC) $(CFLAGS) -c main.c -o main.o

BOOTX64.EFI: main.o
	$(CC) $(LDFLAGS) -o $@ main.o

esp: BOOTX64.EFI
	mkdir -p esp/EFI/BOOT esp/BIYOS/modules
	cp BOOTX64.EFI esp/EFI/BOOT/BOOTX64.EFI
	cp boot.cfg esp/BIYOS/boot.cfg

run: esp
	qemu-system-x86_64 -m 256M -bios $(OVMF) -drive file=fat:rw:esp,format=raw -net none

clean:
	rm -rf main.o BOOTX64.EFI esp
