# BIYOS Test-Kernel (ELF64)
#
# Bauen in WSL (Ubuntu):  sudo apt install gcc make
# (Der normale Linux-gcc erzeugt direkt x86_64-ELF. In MSYS2 gibt es standardmaessig
#  keinen ELF-Compiler, nimm dafuer WSL.)
#
#   make        -> kernel.elf bauen
#   make EXTRA=-DBIYOS_TEST_FAULT=1   -> Testfehler ausloesen (1 = #UD, 2 = #DE, 3 = #PF, 4 = #PF nach unmap)
#   make EXTRA=-DBIYOS_DISK_WRITE_TEST=1   -> Schreibtest auf nvme0p2, stellt den Inhalt wieder her
#                                      (=2: Muster stehen lassen). NUR mit Test-Images!
#   make esp    -> kernel.elf nach ../boot/esp/BIYOS/ kopieren (nach 'make esp' im boot-Ordner)

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mgeneral-regs-only -I../boot
LDFLAGS := -nostdlib -static -no-pie -T linker.ld -z max-page-size=4096 -Wl,--build-id=none

.PHONY: all esp clean

all: kernel.elf

SRCS := kernel.c arch.c mm.c sched.c acpi.c kbd.c pci.c nvme.c blk.c fat32.c vfs.c shell.c isr.S

kernel.elf: $(SRCS) kernel.h kapi.h cpu.h blk.h fs.h linker.ld ../boot/bootinfo.h
	$(CC) $(CFLAGS) $(EXTRA) $(LDFLAGS) -o $@ $(SRCS)

esp: kernel.elf
	mkdir -p ../boot/esp/BIYOS
	cp kernel.elf ../boot/esp/BIYOS/kernel.elf

clean:
	rm -f kernel.elf
