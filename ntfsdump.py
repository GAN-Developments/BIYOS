#!/usr/bin/env python3
"""Kleiner NTFS-Strukturdumper (zum Vergleich von mkntfs und dem BIYOS-Formatierer)."""
import struct, sys
img = sys.argv[1]; off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
nrec = int(sys.argv[3]) if len(sys.argv) > 3 else 30
verbose = len(sys.argv) > 4
f = open(img, 'rb'); f.seek(off); data_bs = f.read(512)
bps = struct.unpack_from('<H', data_bs, 11)[0]; spc = data_bs[13]; cs = bps * spc
mft_lcn = struct.unpack_from('<Q', data_bs, 0x30)[0]
cpr = struct.unpack_from('<b', data_bs, 0x40)[0]
rsz = (cpr * cs) if cpr > 0 else (1 << -cpr)
def rd(o, n):
    f.seek(off + o); return f.read(n)
def fix(buf):
    buf = bytearray(buf)
    uo, uc = struct.unpack_from('<HH', buf, 4)
    usn = buf[uo:uo+2]
    for i in range(1, uc):
        p = i * 512 - 2
        if buf[p:p+2] != usn: return None
        buf[p:p+2] = buf[uo+2*i:uo+2*i+2]
    return bytes(buf)
def runs(b):
    out = []; i = 0; lcn = 0
    while i < len(b) and b[i]:
        lo = b[i] & 15; ho = b[i] >> 4; i += 1
        ln = int.from_bytes(b[i:i+lo], 'little'); i += lo
        if ho:
            dl = int.from_bytes(b[i:i+ho], 'little', signed=True); i += ho; lcn += dl; out.append((lcn, ln))
        else: out.append((None, ln))
    return out
NAMES = {0x10:'$STD_INFO',0x20:'$ATTR_LIST',0x30:'$FILE_NAME',0x40:'$OBJECT_ID',0x50:'$SECURITY_DESC',0x60:'$VOLUME_NAME',0x70:'$VOLUME_INFO',0x80:'$DATA',0x90:'$INDEX_ROOT',0xA0:'$INDEX_ALLOC',0xB0:'$BITMAP',0xC0:'$REPARSE',0xD0:'$EA_INFO',0xE0:'$EA',0x100:'$LOGGED_UTIL'}
rec0 = fix(rd(mft_lcn * cs, rsz))
print("bps", bps, "spc", spc, "rec size", rsz, "mft lcn", mft_lcn)
for n in range(nrec):
    r = fix(rd(mft_lcn * cs + n * rsz, rsz))
    if r is None or r[:4] != b'FILE':
        print(f"[{n}] -- no record ({r[:4] if r else None})"); continue
    seq, links, aoff, flags, used, alloc = struct.unpack_from('<HHHHII', r, 16)
    base = struct.unpack_from('<Q', r, 32)[0]; nextid = struct.unpack_from('<H', r, 40)[0]
    print(f"[{n}] seq={seq} links={links} flags={flags:#x} used={used} alloc={alloc} base={base:#x} usa_cnt={struct.unpack_from('<H', r, 6)[0]} lsn={struct.unpack_from('<Q', r, 8)[0]}")
    p = aoff
    while p < used:
        t = struct.unpack_from('<I', r, p)[0]
        if t == 0xFFFFFFFF: break
        ln = struct.unpack_from('<I', r, p + 4)[0]; nr = r[p + 8]; nl = r[p + 9]; fl = struct.unpack_from('<H', r, p + 12)[0]; aid = struct.unpack_from('<H', r, p + 14)[0]
        nm = r[p + struct.unpack_from('<H', r, p + 10)[0]: p + struct.unpack_from('<H', r, p + 10)[0] + nl*2].decode('utf-16le') if nl else ''
        desc = f"   {NAMES.get(t, hex(t)):15} len={ln:4} {'NR' if nr else 'R '} id={aid} flags={fl:#x} name='{nm}'"
        if nr:
            sv, ev, ro = struct.unpack_from('<QQH', r, p + 16)[0:3]
            alloc_s, real_s, init_s = struct.unpack_from('<QQQ', r, p + 40)
            desc += f" vcn={sv}-{ev} alloc={alloc_s} real={real_s} init={init_s} runs={runs(r[p+ro:p+ln])}"
        else:
            vl, vo = struct.unpack_from('<IH', r, p + 16)
            desc += f" vlen={vl} voff={vo} rflags={r[p+22]}"
            v = r[p + vo: p + vo + vl]
            if t == 0x10: desc += f" std={vl}B " + (f"owner={struct.unpack_from('<I', v, 48)[0]} sec_id={struct.unpack_from('<I', v, 52)[0]:#x} ver={struct.unpack_from('<I', v, 40)[0]}" if vl >= 72 else '') + f" fileflags={struct.unpack_from('<I', v, 32)[0]:#x}"
            if t == 0x30: 
                fl2 = struct.unpack_from('<I', v, 56)[0]; l2 = v[64]; ns = v[65]; nm2 = v[66:66+l2*2].decode('utf-16le')
                desc += f" parent={struct.unpack_from('<Q', v, 0)[0]&0xFFFFFFFFFFFF:#x}/{struct.unpack_from('<Q', v, 0)[0]>>48} ns={ns} name={nm2!r} flags={fl2:#x} alloc={struct.unpack_from('<Q', v, 40)[0]} real={struct.unpack_from('<Q', v, 48)[0]}"
            if t == 0x70: desc += f" ver={v[8]}.{v[9]} vflags={struct.unpack_from('<H', v, 10)[0]:#x}"
            if t == 0x60: desc += f" name={v.decode('utf-16le')!r}"
            if t == 0x90 or verbose: desc += " " + v[:96].hex()
        print(desc)
        p += ln
