# SPDX-License-Identifier: MIT
# Baut die Treiber-Module (DRIV) und legt sie auf die ESP: ../boot/esp/BIYOS/modules/
# Reihenfolge: make -C boot && make -C kernel && make -C modules (die Module brauchen kapi.h aus ../kernel).
# Braucht gcc, ld, objcopy und python3. Die ESP-Verzeichnisse legt "make -C boot esp" an.
CC      ?= gcc
CFLAGS  := -c -Os -Wall -Wextra -ffreestanding -fPIE -fno-stack-protector -mno-red-zone -mgeneral-regs-only \
           -fno-builtin -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -fvisibility=hidden -I../kernel
ESPDIR  := ../boot/esp/BIYOS/modules
MODULES := usbhid ahci

all: $(MODULES:%=%.drv)

# Version steht im Header des Moduls (zweites Argument von make-module.py)
%.drv: %.c ../kernel/kapi.h ../kernel/blk.h ../kernel/fs.h ../tools/exec-tests/flat.ld
	$(CC) $(CFLAGS) -o $*.o $<
	ld -T ../tools/exec-tests/flat.ld -o $*.elf $*.o 2>/dev/null
	objcopy -O binary $*.elf $*.bin
	python3 ../tools/make-module.py $@ DRIV 1 --bin $*.bin

esp: all
	mkdir -p $(ESPDIR)
	cp $(MODULES:%=%.drv) $(ESPDIR)/

clean:
	rm -f *.o *.elf *.bin *.drv

.PHONY: all esp clean
