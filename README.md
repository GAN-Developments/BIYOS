# BIYOS — Build It Yourself Operating System

[Deutsche Version](README.de.md) · MIT licensed · **alpha / hobby project**

BIYOS is a small x86_64 operating system written from scratch: its own UEFI bootloader, its own
kernel, its own drivers, filesystems and shell — no libraries, no borrowed kernel. It is built around
one unusual idea:

> **Programs get as much control over the machine as possible.** Everything runs in ring 0 in a single
> flat address space. Every page is readable, writable and executable. There is no memory protection,
> no system-call boundary, and every kernel service is a function pointer that any program may replace.

It is an educational and experimental system — a playground for people who want to see (and change)
what is underneath. It is **not** a secure or production operating system, and it is not meant to be.

![BIYOS shell browsing an NTFS volume](docs/screenshot-ntfs.png)

## Status

| Area | State |
|---|---|
| UEFI bootloader (x86_64, PE32+), boot menu, ELF64 kernel loading, module loading | working |
| GDT/TSS, IDT, exception dump (register state, page-fault address), identity-mapped paging (WB/WC/UC per region, no NX) | working |
| Physical page allocator, kernel heap, `paging_map` / `paging_unmap` | working |
| Kernel function table `kapi` (console, memory, paging, tasks, input, files) — replaceable at runtime | working |
| ACPI MADT parsing, IOAPIC routing, Local-APIC timer (PIT-calibrated, 100 Hz) | working |
| Preemptive round-robin scheduler (tasks, `sleep`, `yield`) | working |
| PS/2 keyboard (German and US layout), scrolling framebuffer console, COM1 mirror | working |
| PCI scan, **NVMe driver**, block layer, GPT/MBR partitions, filesystem detection | working |
| **FAT32**: read/write, long file names, directories, move/rename | working |
| **NTFS**: read, plus **experimental write** (create/modify/append/delete/rename, mkdir, hard links, named streams, 900-file directories) | working, experimental |
| Virtual file system with drive letters (`C:` = first NTFS volume, `E:`… = FAT32), per-drive working directory | working |
| Emergency shell with `peek`/`poke`, `hexwrite`, file commands | working |
| Executable format (`DRIV` / `STRT` / `PLSE`), loader (`run`, `start`), kernel modules (`module load/remove`, also from `boot.cfg`), `IMPORT` section | working |
| `C:\Start` (auto-run `STRT` programs), `edit` (nano-like, with `-hex` view) | planned |
| NTFS write support, AHCI/SATA, USB (xHCI/HID), ACPI power-off, SMP, networking | planned |

Everything above was developed and tested in **QEMU + OVMF**. It has **not** been tested on real hardware.

## Safety — please read

* BIYOS deliberately has no protection. A buggy program can overwrite the kernel or any disk it can reach.
* **Run it in a virtual machine** or on a spare machine. Do not point it at a disk with data you care about.
* To limit accidents, **all volumes are mounted read-only** automatically. Writing must be enabled
  explicitly per drive (`mount nvme0p1 e rw`).
* **NTFS writing is experimental.** BIYOS refuses it unless the volume is clean (no dirty flag, clean journal, no
  Windows hibernation/Fast Startup file). It does not write the NTFS journal: like ntfs-3g it resets `$LogFile` on the first
  write and keeps the dirty flag set while an operation runs. It was verified with `ntfs-3g`/`ntfsresize` in QEMU,
  **never with Windows/chkdsk**. Do not use it on a Windows system volume; use a throw-away NTFS image or data partition.
* The drivers never write on their own. The only disk-write self-test is a compile-time option
  (`BIYOS_DISK_WRITE_TEST`) meant for throw-away test images.
* Windows volumes: BitLocker volumes are detected but not readable. A Windows "Fast Startup"
  (hibernated) NTFS volume can show stale data because the NTFS journal is not replayed.
* The bootloader is unsigned — Secure Boot must be off to try it on real firmware.
* MIT license: provided "as is", without warranty of any kind.

## Quick start (Ubuntu / WSL)

```sh
sudo apt install build-essential gcc-mingw-w64-x86-64 make qemu-system-x86 ovmf \
                 gdisk dosfstools mtools python3

make -C boot esp                       # UEFI bootloader (MinGW cross compiler → BOOTX64.EFI) + ESP folder
make -C kernel && make -C kernel esp   # kernel (ELF64) → boot/esp/BIYOS/kernel.elf
sh tools/make-test-image.sh nvme.img     # 128 MiB GPT disk image: FAT32 partition + placeholder NTFS partition
make -C boot run NVME_IMG=../nvme.img  # boot in QEMU with OVMF and an emulated NVMe drive
```

If OVMF lives elsewhere, pass `OVMF=/path/to/OVMF.fd`. For a populated *real* NTFS test volume
(directories, hard links, streams, a 600-file index, sparse file) use `sh tools/make-ntfs-test-image.sh`
(needs `ntfs-3g` with FUSE).

On boot, press any key within 3 seconds for the boot menu (normal / without modules / choose modules).
You land in the shell. Try:

```
BIYOS E:\> mount                       # C: = NTFS, E: = FAT32, both read-only
BIYOS E:\> c:
BIYOS C:\> dir
BIYOS C:\> type hallo.txt:geheim       # NTFS named stream
BIYOS C:\> mount nvme0p1 e rw          # allow writing to the FAT32 volume
BIYOS C:\> hexwrite 00 01 02 "Hello" > e:test.bin
BIYOS C:\> copy c:big.bin e:           # copy between filesystems
BIYOS C:\> peek 100000 20              # raw memory dump
```

### Booting on real hardware (at your own risk)

Create a FAT32 EFI System Partition on a **USB stick** and copy: `BOOTX64.EFI` → `EFI/BOOT/`,
`kernel.elf` and `boot.cfg` → `BIYOS/`. Disable Secure Boot, boot the stick. Use a machine you can afford to
break. The PS/2 keyboard driver works with internal laptop keyboards; USB keyboards are not supported yet.

## Shell commands

| Command | Description |
|---|---|
| `help` | list commands |
| `dir`/`ls`, `cd`, `pwd`, `type`/`cat`, `mkdir`, `del`/`rm`, `copy`/`cp`, `ren`/`mv`, `df` | files and directories (`/` or `\`, optional drive, case-insensitive) |
| `E:` | change drive |
| `mount [<dev> <letter> [rw]]`, `umount <letter>` | drives; automounted read-only |
| `echo text > file`, `>>` | write / append text (quote names with spaces) |
| `hexwrite 00 01 "Hello" > file` | write raw bytes and ASCII; hex tokens may be `0A`, `0xFF`, `DEADBEEF` |
| `peek <addr> [len]`, `poke <addr> <value> [1\|2\|4\|8]` | read / write any memory (hex) |
| `pci`, `disks`, `diskread <dev> <lba>` | hardware and block devices |
| `run <file> [args]`, `start <file> [args]` | run a `PLSE`/`STRT` program and wait / as its own task; typing the name of an executable file does `run` |
| `module list`, `module load <file.drv>`, `module remove <name\|nr>` | kernel modules (`DRIV`); modules from `boot.cfg` start after the drives are mounted |
| `mem`, `uptime`, `tasks`, `ver`, `layout de\|us` | system info |
| `reboot`, `shutdown` | restart; power-off currently works in VMs only |

## How it works

**Boot.** `boot/` is a UEFI application built with MinGW. It reads `\BIYOS\boot.cfg`, shows a menu on a key
press, loads the ELF64 kernel to its link address (1 MiB), loads modules, collects the GOP framebuffer,
the ACPI RSDP and the memory map, calls `ExitBootServices` and jumps to the kernel with a `BootInfo`
pointer (System V ABI).

**Kernel.** `kernel/` is freestanding C. After switching to its own stack it sets up GDT/TSS, IDT, and flat
identity-mapped page tables, then the memory manager, timer/scheduler, keyboard, storage and the shell.
Exceptions print the full register state and halt instead of silently rebooting.

**The function table.** All replaceable services live in the `KernelApi` struct (`kernel/kapi.h`). The
kernel itself calls them through that table, so a program can substitute, wrap or extend a service:

```c
void *(*orig)(size_t) = kapi.malloc;
kapi.malloc = my_counting_malloc;      // every allocation now goes through your code
```

Because everything is ring 0 and writable, programs can also patch kernel code directly, build their own
page tables and load `cr3`, or take over the timer. That is the point.

**Storage stack.** `PCI → NVMe → BlockDev (disk + partitions, GPT/MBR) → Fs (FAT32 / NTFS) → VFS`.
The VFS maps drive letters, keeps a working directory per drive and refuses writes on read-only mounts.
Filesystem and block-device operations are function pointers too.

### Executable format

BIYOS executables start with a fixed 20-byte header (all numbers big-endian, so they stay readable in a hex dump):

| Offset | Size | Content |
|---|---|---|
| 0 | 1 | `0xAA` |
| 1 | 3 | ASCII `EXE` |
| 4 | 4 | type: `DRIV` (driver/kernel module), `STRT` (run at boot), `PLSE` ("please execute", normal program) |
| 8 | 4 | version |
| 12 | 8 | skipped |

An optional **`IMPORT` section** may follow directly (`"IMPORT"`, then a 4-byte length of the entries). Each entry is
`X` (4), `Y` (4), `TARGET` (4), `NL` (1), `NAME` (`NL` bytes): *"bytes X…Y-1 of file NAME are a function; copy them to
offset TARGET of this program"*. The program reserves that space itself (NOPs, zeros) and calls it with a normal
`call rel32`, so there is no GOT/PLT. The loader copies the bytes in before the program starts. A name without a path
is looked up next to the executable first, then in the current directory. Imported bytes must be position-independent
and follow the System V ABI.

The code starts right after the header (and the `IMPORT` section) and is the entry point. The whole file is loaded to a
16-byte aligned heap address and executed in place, so code must be position-independent.

```
int entry(KernelApi *api, int argc, char **argv);     // System V ABI; return 0 = ok (a bare `ret` works)
```

* `PLSE`/`STRT`: `argv[0]` is the path as typed. `run` executes in the calling task and frees the program afterwards;
  `start` creates a task and frees the program when it returns. A program that calls `kapi.task_exit()` instead stays
  resident.
* `DRIV`: called with `argc = 0` when loaded and `argc = -1` when removed (return non-zero to refuse removal). A module
  can add shell commands with `kapi.cmd_register`; they disappear when the module is removed. Anything else it installs
  (interrupt handlers, tasks, replaced `kapi` entries) it must undo itself.
* `tools/make-module.py` builds executables (including `IMPORT` sections) for tests; `tools/exec-tests/` has small
  assembly examples (`hello`, `args`, `call` with an import, `bg`, and drivers `hi`, `fail`, `busy`).

## Repository layout

```
boot/      UEFI bootloader (main.c, efi.h, bootinfo.h, Makefile, boot.cfg)
kernel/    kernel.c        entry, console, self-tests         arch.c   GDT/TSS/IDT/paging/exceptions
           isr.S           interrupt stubs                     mm.c     page allocator, heap, paging API
           sched.c         LAPIC timer + scheduler             acpi.c   MADT, IOAPIC
           kbd.c           PS/2 keyboard                       shell.c  emergency shell
           pci.c nvme.c blk.c   storage                        fat32.c ntfs.c vfs.c   filesystems
           exec.c          executable loader, kernel modules, IMPORT
           kapi.h          kernel function table (what programs include)
tools/     make-test-image.sh, make-ntfs-test-image.sh, make-module.py, qemu_shell_test.py, exec-tests/
docs/      screenshots
```

## Testing

Tests run the real kernel in QEMU. `tools/qemu_shell_test.py` boots it, types shell commands through the QEMU
monitor, and prints the serial transcript:

```sh
python3 tools/qemu_shell_test.py --image nvme.img --cmd "disks" --cmd "dir e:" --cmd "type e:hallo.txt"
```

Filesystem work is verified from the host with independent tools: `fsck.vfat`, `mtools` and the `ntfs-3g`
utilities (e.g. BIYOS copies a 5 MB fragmented NTFS file with an attribute list to FAT32 and the result is
compared byte for byte). Kernel build flags for tests:

* `make -C kernel EXTRA=-DBIYOS_TEST_FAULT=1|2|3|4` — trigger `#UD`, `#DE`, `#PF`, `#PF` after `unmap`
* `make -C kernel EXTRA=-DBIYOS_DISK_WRITE_TEST=1` — block-layer write test on partition `nvme0p2`
  (**test images only**; `=2` leaves the pattern for host-side checking)
* `make -C kernel EXTRA="-DBIYOS_FS_STRESS_TEST=1 [-DBIYOS_STRESS_DRIVE=\'E\'] [-DBIYOS_STRESS_MANY=900]"` — filesystem
  stress test (create/append/rename/move/delete, 3 MB file, nested directories, optional 900-file directory) on drive
  `C:` by default; verifies every byte itself. **Test images only.** Afterwards check the image from the host
  (`ntfsresize --info --force`, `fsck.vfat`, mount with `ntfs-3g`/`mtools` and compare contents).

## Known limitations

No ring 3 / isolation (by design), no SMP, no USB, no AHCI, ASCII-only file names, only one NVMe namespace,
FPU/SSE state is not saved on task switches, FAT32/NTFS timestamps are not preserved by rename/copy,
write operations are not crash-safe, console font has uppercase letters only, and kernel/shell
messages are currently in German (English strings are a welcome contribution).
NTFS writing: files with an `$ATTRIBUTE_LIST`, compressed or encrypted files are refused; new files inherit the
directory's security ID (no ACL handling); directory indexes are rebuilt on every change (O(n) per change);
the MFT can grow only while its bitmap fits in its allocated cluster (about 32,000 files on a 4 KiB-cluster volume).

## Roadmap

1. Installer (English/German, selectable), bilingual kernel/shell texts, bootable ISO
2. ~~Executable loader, kernel modules, `IMPORT` linking~~ (done)
3. `C:\Start` folder: run every `STRT` program at boot; the shell becomes a replaceable program
4. `edit` — nano-like editor with a `-hex` mode (needs arrow keys and cell access in the console)
5. AHCI/SATA, USB (xHCI + HID), ACPI power management

## Contributing

Issues and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). Because the project is about
being hackable, readable code with explanatory comments is preferred over cleverness.

## License

[MIT](LICENSE)
