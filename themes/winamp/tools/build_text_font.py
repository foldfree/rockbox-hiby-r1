#!/usr/bin/env python3
# Build a Rockbox BDF from Winamp's TEXT.BMP (5x6 monospace bitmap font).
import subprocess

SRC = "/work/fontbuild/TEXT.BMP"
OUT = "/work/fontbuild/winamp_text.bdf"
CW, CH = 5, 6
SCALE = 2
TW, TH = CW*SCALE, CH*SCALE   # 10x12

# (row, col) -> codepoint   [verified against an ASCII dump of TEXT.BMP]
charmap = {}
for i, c in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
    charmap[(0, i)] = ord(c)
charmap[(0, 26)] = ord('"')
charmap[(0, 27)] = ord('@')
for i, c in enumerate("0123456789"):
    charmap[(1, i)] = ord(c)
charmap[(1, 10)] = 0x2026  # … (this hidden cell shifts the rest of row 1)
row1_rest = {11: '.', 12: ':', 13: '(', 14: ')', 15: '-', 16: "'", 17: '!',
             18: '_', 19: '+', 20: '\\', 21: '/', 22: '[', 23: ']', 24: '^',
             25: '&', 26: '%', 27: ',', 28: '=', 29: '$', 30: '#'}
for col, ch in row1_rest.items():
    charmap[(1, col)] = ord(ch)
charmap[(2, 0)] = 0xC5  # Å
charmap[(2, 1)] = 0xD6  # Ö
charmap[(2, 2)] = 0xC4  # Ä
charmap[(2, 3)] = ord('?')
charmap[(2, 4)] = ord('*')

def load(path):
    out = subprocess.check_output(["convert", path, "txt:-"]).decode()
    px = {}
    for line in out.splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        coord, rest = line.split(":", 1)
        try:
            x, y = (int(v) for v in coord.split(","))
        except ValueError:
            continue
        h = rest.split("#")[1][:6]
        px[(x, y)] = (int(h[0:2],16), int(h[2:4],16), int(h[4:6],16))
    return px

px = load(SRC)
def lit(x, y):
    r, g, b = px.get((x, y), (0,0,0))
    return 1 if (g >= 140 and g > r+40 and g > b+40) else 0

def glyph(row, col):
    out = []
    for oy in range(TH):
        sy = row*CH + oy//SCALE
        r = []
        for ox in range(TW):
            sx = col*CW + ox//SCALE
            r.append(lit(sx, sy))
        out.append(r)
    return out

glyphs = {}   # codepoint -> rows
for (row, col), cp in charmap.items():
    glyphs[cp] = glyph(row, col)
# space
glyphs[0x20] = [[0]*TW for _ in range(TH)]
# lowercase -> uppercase bitmaps
for c in range(ord('a'), ord('z')+1):
    glyphs[c] = glyphs[c - 32]

def row_hex(row):
    nb = (TW+7)//8
    val = 0
    for i in range(nb*8):
        val = (val<<1) | (row[i] if i < TW else 0)
    return "%0*X" % (nb*2, val)

codes = sorted(glyphs)
with open(OUT, "w") as f:
    f.write("STARTFONT 2.1\n")
    f.write("FONT -winamp-text-medium-r-normal--%d-%d0-75-75-c-%d0-iso10646-1\n" % (TH, TH, TW))
    f.write("SIZE %d 75 75\n" % TH)
    f.write("FONTBOUNDINGBOX %d %d 0 0\n" % (TW, TH))
    f.write("STARTPROPERTIES 3\nFONT_ASCENT %d\nFONT_DESCENT 0\nDEFAULT_CHAR 32\nENDPROPERTIES\n" % TH)
    f.write("CHARS %d\n" % len(codes))
    for cp in codes:
        rows = glyphs[cp]
        f.write("STARTCHAR U+%04X\nENCODING %d\n" % (cp, cp))
        f.write("SWIDTH %d 0\nDWIDTH %d 0\nBBX %d %d 0 0\nBITMAP\n" % (int(TW*1000/TH), TW, TW, TH))
        for r in rows:
            f.write(row_hex(r) + "\n")
        f.write("ENDCHAR\n")
    f.write("ENDFONT\n")

# preview a few
for cp in [ord('A'), ord('W'), ord('5'), ord('('), ord('-')]:
    print(chr(cp))
    for r in glyphs[cp]:
        print("".join("#" if v else "." for v in r))
print("wrote", OUT, "glyphs", len(codes))
