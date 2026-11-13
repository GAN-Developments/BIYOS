# Contributing to BIYOS

Thanks for your interest! BIYOS is a hobby/educational operating system, so the bar is simple:
**the code should be readable and the behaviour should be tested.**

## Ground rules

* Keep the project's idea intact: ring 0 only, no memory protection, every kernel service reachable (and
  replaceable) through `kapi`. Proposals that add isolation are better as optional modules.
* Be careful with anything that writes to disks. Drivers must never write on their own; mounts stay read-only
  until the user asks for `rw`. Test writes on throw-away images only.
* Prefer small, commented, boring C over clever C. Explain *why* in comments (hardware quirks, spec sections).
* No external libraries in the kernel or bootloader. Freestanding C only (`-ffreestanding`, no libc).

## Build and test

See the *Quick start* in [README.md](README.md). Before opening a pull request:

1. Build `boot/` and `kernel/` cleanly (no warnings with `-Wall -Wextra`).
2. Boot in QEMU and run the relevant shell commands, e.g. with `tools/qemu_shell_test.py`.
3. For filesystem changes verify the image from the host with independent tools (`fsck.vfat`, `mtools`,
   `ntfs-3g` utilities) and compare file contents byte for byte.
4. Describe what you tested and on which QEMU/OVMF versions. Say clearly if you tried real hardware.

## Style

* 4-space indentation, `snake_case`, fixed-width integer types, one driver per file.
* Every source file starts with `SPDX-License-Identifier: MIT` and a short comment on what it does.
* Shell and console output is currently German/uppercase because of the tiny built-in font; user-facing
  text translations are welcome once the font has lowercase letters.

## License

By contributing you agree that your contributions are licensed under the MIT License (see `LICENSE`).
