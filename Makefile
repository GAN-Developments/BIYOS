# BIYOS Test-Kernel (ELF64)
#
# Bauen in WSL (Ubuntu):  sudo apt install gcc make
# (Der normale Linux-gcc erzeugt direkt x86_64-ELF. In MSYS2 gibt es standardmaessig
#  keinen ELF-Compiler, nimm dafuer WSL.)
#
#   make        -> kernel.elf bauen
#   make esp    -> kernel.elf nach ../boot/esp/BIYOS/ kopieren (nach 'make esp' im boot-Ordner)

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mgeneral-regs-only -I../boot
LDFLAGS := -nostdlib -static -no-pie -T linker.ld -z max-page-size=4096

.PHONY: all esp clean

all: kernel.elf

kernel.elf: kernel.c linker.ld ../boot/bootinfo.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ kernel.c

esp: kernel.elf
	mkdir -p ../boot/esp/BIYOS
	cp kernel.elf ../boot/esp/BIYOS/kernel.elf

clean:
	rm -f kernel.elf
