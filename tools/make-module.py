#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Schreibt ein BIYOS-Executable mit gueltigem Header (fuer Tests).

Header: 0xAA, "EXE", Typ (DRIV/STRT/PLSE), 4 Byte Version (Big Endian), 8 Byte frei.
Danach optional ein IMPORT-Abschnitt und der Code (Hex-Bytes und/oder eine Binaerdatei).

Beispiele:
  make-module.py out.drv DRIV 1 C3
  make-module.py out.plse PLSE 1 --bin code.bin
  make-module.py out.plse PLSE 1 --bin code.bin --import ADD.BIN:0:4:+37

--import NAME:X:Y:ZIEL   Die Bytes X..Y-1 (Y exklusiv) der Datei NAME werden beim Laden an die Stelle
                         ZIEL im Executable kopiert. ZIEL ist ein Offset ab Dateianfang, oder mit
                         vorangestelltem '+' ein Offset ab dem ersten Codebyte (ohne Header und IMPORT).
                         Zahlen duerfen dezimal oder 0x-hex sein. Mehrfach angebbar.
"""
import argparse, struct, sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("datei")
ap.add_argument("typ", choices=["DRIV", "STRT", "PLSE"])
ap.add_argument("version", type=lambda s: int(s, 0))
ap.add_argument("hex", nargs="*", help="Code-Bytes in Hex, z.B. C3")
ap.add_argument("--bin", help="Binaerdatei, wird hinter die Hex-Bytes als Code angehaengt")
ap.add_argument("--import", dest="imports", action="append", default=[], metavar="NAME:X:Y:ZIEL")
a = ap.parse_args()

code = bytes(int(b, 16) for b in a.hex)
if a.bin:
    code += open(a.bin, "rb").read()

entries = []
for spec in a.imports:
    name, x, y, dest = spec.rsplit(":", 3)
    entries.append((name.encode("ascii"), int(x, 0), int(y, 0), dest))

# Laenge des IMPORT-Abschnitts steht vor den Eintraegen und bestimmt, wo der Code beginnt
import_len = sum(13 + len(e[0]) for e in entries)
code_start = 20 + (10 + import_len if entries else 0)

data = b"\xAA" + b"EXE" + a.typ.encode() + struct.pack(">I", a.version) + b"\x00" * 8
if entries:
    data += b"IMPORT" + struct.pack(">I", import_len)
    for name, x, y, dest in entries:
        d = code_start + int(dest[1:], 0) if dest.startswith("+") else int(dest, 0)
        data += struct.pack(">III", x, y, d) + bytes([len(name)]) + name
data += code
open(a.datei, "wb").write(data)
print(f"{a.datei}: {len(data)} Bytes, Code ab Byte {code_start}")
