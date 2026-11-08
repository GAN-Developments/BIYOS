#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot BIYOS in QEMU, type shell commands through the QEMU monitor, print the serial transcript.

Example:
    python3 tools/qemu_shell_test.py --image nvme.img --cmd "disks" --cmd "dir e:" --cmd "type e:hallo.txt"

Needs: qemu-system-x86_64, OVMF, the MinGW cross compiler and gcc (to build), optionally Pillow for PNG
screenshots. The guest keyboard layout defaults to German (--layout us for the US layout).
"""
import argparse, os, re, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DE = {' ': 'spc', '\n': 'ret', '-': 'slash', '_': 'shift-slash', '.': 'dot', ',': 'comma', ':': 'shift-dot',
      '"': 'shift-2', '>': 'shift-less', '/': 'shift-7', '=': 'shift-0'}
US = {' ': 'spc', '\n': 'ret', '-': 'minus', '_': 'shift-minus', '.': 'dot', ',': 'comma', ':': 'shift-semicolon',
      '"': 'shift-apostrophe', '>': 'shift-dot', '/': 'slash', '=': 'equal'}


def qemu_key(c, layout):
    table = DE if layout == 'de' else US
    if c in table:
        return table[c]
    if c.isalpha():
        k = c.lower()
        if layout == 'de':
            k = {'y': 'z', 'z': 'y'}.get(k, k)   # QWERTZ: the physical keys are swapped
        return ('shift-' + k) if c.isupper() else k
    return c


def make_module(path, typ, version):
    with open(path, 'wb') as f:
        f.write(b'\xAA' + b'EXE' + typ + struct.pack('>I', version) + b'\x00' * 8 + b'\xC3')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', help='raw disk image attached as an NVMe drive')
    ap.add_argument('--cmd', action='append', default=[], help='shell command to type (repeatable)')
    ap.add_argument('--ovmf', default=os.environ.get('OVMF', '/usr/share/ovmf/OVMF.fd'))
    ap.add_argument('--layout', choices=['de', 'us'], default='de')
    ap.add_argument('--no-build', action='store_true', help='do not run make for boot/ and kernel/')
    ap.add_argument('--kernel-extra', default='', help='EXTRA= flags for the kernel build, e.g. -DBIYOS_TEST_FAULT=1')
    ap.add_argument('--boot-timeout', type=int, default=60)
    ap.add_argument('--wait-for', default='BIYOS SHELL', help='serial text that means "ready to type"')
    ap.add_argument('--cmd-delay', type=float, default=1.0, help='seconds to wait after each command')
    ap.add_argument('--wait-end', type=float, default=0.0, help='extra seconds before the screenshot (slow copies)')
    ap.add_argument('--screenshot', help='write a screenshot (.png needs Pillow, otherwise .ppm)')
    ap.add_argument('--keep-serial', help='also save the raw serial log to this file')
    a = ap.parse_args()

    boot, kern = os.path.join(ROOT, 'boot'), os.path.join(ROOT, 'kernel')
    if not a.no_build:
        subprocess.run(['make', '-C', boot, 'esp'], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['make', '-C', kern, 'clean'], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['make', '-C', kern, 'EXTRA=' + a.kernel_extra], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['make', '-C', kern, 'esp'], check=True, stdout=subprocess.DEVNULL)
    esp = os.path.join(boot, 'esp')
    mods = os.path.join(esp, 'BIYOS', 'modules')
    os.makedirs(mods, exist_ok=True)
    for name, ver in (('video.drv', 1), ('usbhid.drv', 2)):   # dummy modules listed in boot.cfg
        if not os.path.exists(os.path.join(mods, name)):
            make_module(os.path.join(mods, name), b'DRIV', ver)

    tmp = tempfile.mkdtemp(prefix='biyos-test-')
    serial, sock = os.path.join(tmp, 'serial.txt'), os.path.join(tmp, 'mon.sock')
    cmd = ['qemu-system-x86_64', '-m', '256M', '-bios', a.ovmf, '-drive', f'file=fat:rw:{esp},format=raw',
           '-net', 'none', '-display', 'none', '-serial', f'file:{serial}',
           '-monitor', f'unix:{sock},server,nowait']
    if a.image:
        cmd += ['-drive', f'file={a.image},if=none,id=nv0,format=raw', '-device', 'nvme,drive=nv0,serial=biyos0']
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(a.boot_timeout):
            time.sleep(1)
            if os.path.exists(serial) and a.wait_for.encode() in open(serial, 'rb').read():
                break
        time.sleep(1.5)
        s = socket.socket(socket.AF_UNIX)
        for _ in range(20):
            try:
                s.connect(sock)
                break
            except OSError:
                time.sleep(0.5)
        s.settimeout(2)
        try:
            s.recv(4096)
        except OSError:
            pass

        def mon(c):
            s.send((c + '\n').encode())
            time.sleep(0.12)
            try:
                s.recv(4096)
            except OSError:
                pass

        for line in a.cmd:
            for ch in line + '\n':
                mon('sendkey ' + qemu_key(ch, a.layout))
            time.sleep(a.cmd_delay)
        time.sleep(a.wait_end)
        if a.screenshot:
            ppm = a.screenshot if a.screenshot.endswith('.ppm') else os.path.join(tmp, 'shot.ppm')
            mon('screendump ' + ppm)
            time.sleep(1.5)
            if not a.screenshot.endswith('.ppm'):
                try:
                    from PIL import Image
                    Image.open(ppm).save(a.screenshot)
                except ImportError:
                    print('(Pillow missing: screenshot left as ' + ppm + ')', file=sys.stderr)
    finally:
        q.kill()
        q.wait()

    raw = open(serial, 'rb').read().decode('latin1').replace('\r', '')
    if a.keep_serial:
        open(a.keep_serial, 'w').write(raw)
    txt = re.sub(r'\x1b\[[0-9;=?]*[a-zA-Z]', '', raw)
    txt = re.sub(r'.\x08 \x08', '', txt)       # undo the echoed cursor/backspace sequences
    i = txt.find('BIYOS KERNEL OK')
    print(txt[i:] if i >= 0 else txt)


if __name__ == '__main__':
    main()
