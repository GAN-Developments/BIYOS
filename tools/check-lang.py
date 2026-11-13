#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prueft kernel/lang_en.inc gegen den Quelltext.

  - jeder Schluessel (deutscher Text) muss als Zeichenkette in kernel/*.c oder modules/*.c vorkommen (sonst Tippfehler),
  - doppelte Schluessel werden gemeldet,
  - mit --missing werden Zeichenketten gelistet, die wie deutsche Meldungen aussehen, aber keinen Eintrag haben.
"""
import glob, os, re, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
inc = open(os.path.join(root, 'kernel', 'lang_en.inc')).read()
inc = re.sub(r'/\*.*?\*/', '', inc, flags=re.S)
pairs = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', inc)
keys = [k for k, _ in pairs]
lits = set()
for f in glob.glob(os.path.join(root, 'kernel', '*.c')) + glob.glob(os.path.join(root, 'modules', '*.c')):
    src = open(f).read()
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    lits.update(re.findall(r'"((?:[^"\\\n]|\\.)*)"', src))
bad = 0
for k in keys:
    if k not in lits:
        print('KEIN QUELLTEXT-TREFFER:', k); bad += 1
dup = {k for k in keys if keys.count(k) > 1}
for k in sorted(dup):
    print('DOPPELT:', k); bad += 1
print(len(pairs), 'Eintraege,', bad, 'Probleme')
if '--missing' in sys.argv:
    done = set(keys)
    german = re.compile(r'(UNGUELTIG|FEHLER|KEIN|NICHT|GERAET|LAUFWERK|DATEI|SPEICHER|ZU VIEL|BRAUCHT|WIRD|ODER|UND |FUER|VERZEICHNIS|BEFEHL|ANGABE|ABER|SEITEN|SEKTOR|GESCHRIEBEN|GELADEN|ENTFERNT|STARTEN)')
    for l in sorted(lits - done):
        if german.search(l) and not re.search(r'[a-z]{4}', l.replace('\\n', '')):
            print('OHNE UEBERSETZUNG?', l)
sys.exit(1 if bad else 0)
