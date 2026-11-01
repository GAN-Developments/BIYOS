#!/usr/bin/env python3
"""Schreibt ein BIYOS-Executable mit gueltigem Header (fuer Tests).

Header: 0xAA, "EXE", Typ (DRIV/STRT/PLSE), 4 Byte Version (Big Endian), 8 Byte frei.
Danach optional Code-Bytes in Hex, z.B.:  make-module.py out.drv DRIV 1 C3
"""
import struct, sys

if len(sys.argv) < 4 or sys.argv[2] not in ("DRIV", "STRT", "PLSE"):
    sys.exit("Benutzung: make-module.py <datei> <DRIV|STRT|PLSE> <version> [hex-bytes ...]")
data = b"\xAA" + b"EXE" + sys.argv[2].encode() + struct.pack(">I", int(sys.argv[3])) + b"\x00" * 8
data += bytes(int(b, 16) for b in sys.argv[4:])
open(sys.argv[1], "wb").write(data)
print(f"{sys.argv[1]}: {len(data)} Bytes")
