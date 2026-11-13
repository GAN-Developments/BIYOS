#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Erzeugt kernel/ntfs_tmpl.inc aus einem von mkntfs (ntfs-3g) erzeugten Referenz-Volume.

Der NTFS-Formatierer des Kernels (kernel/mkfs_ntfs.inc) baut alle MFT-Eintraege selbst auf. Nur diese Teile sind feste
Daten und kommen aus der Vorlage: Bootsektor-Vorlage, $AttrDef, die Sicherheitsdeskriptoren samt $Secure-Indexen, die
$UpCase-Tabelle (als kompakte Laufliste) und deren $Info-Eintrag.

  mkntfs -F -Q -L X -s 512 -c 4096 ref.img      (64-MiB-Image)
  python3 tools/gen-ntfs-tmpl.py ref.img > kernel/ntfs_tmpl.inc
"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
cs = 4096
bs = d[:512]
assert struct.unpack_from('<H', bs, 11)[0] == 512 and bs[13] == 8
def fix(buf):
    buf = bytearray(buf); uo, uc = struct.unpack_from('<HH', buf, 4)
    for i in range(1, uc): buf[i*512-2:i*512] = buf[uo+2*i:uo+2*i+2]
    return bytes(buf)
mft = struct.unpack_from('<Q', bs, 0x30)[0] * cs
rec = lambda n: fix(d[mft + n * 1024: mft + (n + 1) * 1024])
def attrs(r):
    p = struct.unpack_from('<H', r, 20)[0]; used = struct.unpack_from('<I', r, 24)[0]
    while p < used:
        t, ln = struct.unpack_from('<II', r, p)
        if t == 0xFFFFFFFF: break
        nr = r[p+8]; nl = r[p+9]; no = struct.unpack_from('<H', r, p+10)[0]
        name = r[p+no:p+no+nl*2].decode('utf-16le')
        if not nr:
            vl, vo = struct.unpack_from('<IH', r, p + 16); val = r[p+vo:p+vo+vl]
        else:
            val = None
        yield t, name, nr, r[p:p+ln], val
        p += ln
def find(n, typ, name=''):
    for t, nm, nr, raw, val in attrs(rec(n)):
        if t == typ and nm == name: return raw, val
    raise KeyError((n, typ, name))
out = []
def arr(name, b, doc):
    out.append(f"/* {doc} ({len(b)} Bytes) */")
    out.append(f"static const uint8_t {name}[{len(b)}] = {{")
    for i in range(0, len(b), 16):
        out.append("    " + ", ".join(f"0x{x:02X}" for x in b[i:i+16]) + ",")
    out.append("};\n")
# Bootsektor (Felder, die der Formatierer setzt, werden dort ueberschrieben)
arr('T_BOOT', bs, 'Bootsektor-Vorlage (aus mkntfs)')
# $AttrDef
lcn = 2054
arr('T_ATTRDEF', d[lcn*cs:lcn*cs+2560], '$AttrDef-Inhalt')
# Sicherheitsdeskriptoren der Systemdateien (aelterer Stil, resident): zwei Varianten
sd_a = find(3, 0x50)[1]
for n in (12, 13, 14, 15): assert find(n, 0x50)[1] == sd_a, n
sd_b = find(4, 0x50)[1]
assert find(7, 0x50)[1] == sd_b
arr('T_SD_A', sd_a, 'Sicherheitsdeskriptor fuer $Volume und die reservierten Eintraege 12-15')
arr('T_SD_B', sd_b, 'Sicherheitsdeskriptor fuer $AttrDef und $Boot')
# Root-SD (nicht resident)
raw = find(5, 0x50)[0]
ro = struct.unpack_from('<H', raw, 32)[0]; real = struct.unpack_from('<Q', raw, 48)[0]
runs = raw[ro:]; ln = runs[0] & 15; l = int.from_bytes(runs[1:1+ln], 'little'); of = (runs[0] >> 4); lc = int.from_bytes(runs[1+ln:1+ln+of], 'little')
arr('T_SD_ROOT', d[lc*cs:lc*cs+real], 'Sicherheitsdeskriptor des Wurzelverzeichnisses')
# $Secure: $SDS-Eintrag und die Index-Wurzeln
raw = find(9, 0x80, '$SDS')[0]
ro = struct.unpack_from('<H', raw, 32)[0]; runs = raw[ro:]; ln = runs[0] & 15; of = runs[0] >> 4
lc = int.from_bytes(runs[1+ln:1+ln+of], 'little')
sds = d[lc*cs:lc*cs+0xFC]            # zwei Deskriptoren (IDs 0x100 und 0x101), je 0x7C Bytes, 16-Byte-ausgerichtet
assert d[lc*cs+0x40000:lc*cs+0x40000+0xFC] == sds
assert struct.unpack_from('<Q', raw, 48)[0] == 0x40000 + 0xFC
arr('T_SDS_ENTRY', sds, '$Secure: die beiden Eintraege in $SDS (stehen bei 0 und gespiegelt bei 0x40000)')
arr('T_SDH_ROOT', find(9, 0x90, '$SDH')[1], '$Secure: INDEX_ROOT von $SDH')
arr('T_SII_ROOT', find(9, 0x90, '$SII')[1], '$Secure: INDEX_ROOT von $SII')
# $UpCase-$Info
arr('T_UPCASE_INFO', find(10, 0x80, '$Info')[1], '$UpCase: $Info')
# $UpCase-Tabelle als Laufliste {Start, Anzahl, Delta, Schritt}
raw = find(10, 0x80)[0]
ro = struct.unpack_from('<H', raw, 32)[0]; runs = raw[ro:]; ln = runs[0] & 15; of = runs[0] >> 4
lc = int.from_bytes(runs[1+ln:1+ln+of], 'little')
up = struct.unpack('<65536H', d[lc*cs:lc*cs+131072])
rl = []
covered = [False] * 65536
for c in range(65536):
    delta = up[c] - c
    if delta == 0 or covered[c]: continue
    best = (1, 0)
    for step in (1, 2):
        n = 0
        while c + n * step < 65536 and not covered[c + n * step] and up[c + n * step] - (c + n * step) == delta: n += 1
        if n > best[1] or (n == best[1] and step < best[0]): best = (step, n)
    step, n = best
    for k in range(n): covered[c + k * step] = True
    rl.append((c, n, delta, step))
# nachpruefen
chk = list(range(65536))
for s, n, dl, st in rl:
    for k in range(n): chk[s + k * st] = s + k * st + dl
assert tuple(chk) == up, "Laufliste stimmt nicht"
out.append(f"/* $UpCase als Laufliste: Zeichen start + k*step (k < count) werden um delta verschoben, alle anderen bleiben (Eintraege: {len(rl)}) */")
out.append("static const struct { uint16_t start, count; int16_t delta; uint16_t step; } T_UPCASE_RUNS[] = {")
for s, n, dl, st in rl:
    out.append(f"    {{0x{s:04X}, {n}, {dl}, {st}}},")
out.append("};\n")
sys.stdout.write("/* SPDX-License-Identifier: MIT */\n/* Automatisch erzeugt von tools/gen-ntfs-tmpl.py aus einem mkntfs-Volume. Nicht von Hand aendern. */\n" + "\n".join(out))
