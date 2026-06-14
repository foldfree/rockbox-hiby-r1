#!/usr/bin/env python3
# Build a Rockbox BDF for the Winamp green LCD digits from NUMBERS.BMP.
# Extract digits 0-9 (9x13 each), scale ~x1.77 to 16x23, synthesize ':' '-' ' '.
import subprocess, sys

SRC = "/work/fontbuild/NUMBERS.BMP"
OUT = "/work/fontbuild/winamp_lcd.bdf"

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

def lit(px, x, y):
    r, g, b = px.get((x, y), (0,0,0))
    return 1 if (g >= 180 and r < 90 and b < 90) else 0

px = load(SRC)
CW, CH = 9, 13          # source cell
TW, TH = 16, 23         # target cell (scaled ~x1.77)

def scale_glyph(bits):  # nearest-neighbour CWxCH -> TWxTH
    out = []
    for oy in range(TH):
        sy = min(CH-1, oy * CH // TH)
        row = []
        for ox in range(TW):
            sx = min(CW-1, ox * CW // TW)
            row.append(bits[sy][sx])
        out.append(row)
    return out

glyphs = {}  # codepoint -> (width, rows[list of list of 0/1, height TH])

# digits 0-9
for d in range(10):
    src = [[lit(px, d*CW + x, y) for x in range(CW)] for y in range(CH)]
    glyphs[ord("0")+d] = (TW, scale_glyph(src))

def blank(w):
    return (w, [[0]*w for _ in range(TH)])

def dot_rect(rows, x0, y0, w, h):
    for y in range(y0, y0+h):
        for x in range(x0, x0+w):
            rows[y][x] = 1

# colon ':'  width 8, two 3x3 dots
cw = 8
crows = [[0]*cw for _ in range(TH)]
dot_rect(crows, 2, 6, 3, 3)
dot_rect(crows, 2, 14, 3, 3)
glyphs[ord(":")] = (cw, crows)

# space
glyphs[ord(" ")] = blank(8)

# minus '-' width 12, bar 10x3 centred
mw = 12
mrows = [[0]*mw for _ in range(TH)]
dot_rect(mrows, 1, 10, 10, 3)
glyphs[ord("-")] = (mw, mrows)

def row_to_hex(row, w):
    nbytes = (w + 7)//8
    val = 0
    for i in range(nbytes*8):
        val = (val<<1) | (row[i] if i < w else 0)
    return ("%0*X" % (nbytes*2, val))

codes = sorted(glyphs)
with open(OUT, "w") as f:
    f.write("STARTFONT 2.1\n")
    f.write("FONT -winamp-lcd-medium-r-normal--23-230-75-75-c-160-iso10646-1\n")
    f.write("SIZE 23 75 75\n")
    f.write("FONTBOUNDINGBOX %d %d 0 0\n" % (TW, TH))
    f.write("STARTPROPERTIES 3\n")
    f.write("FONT_ASCENT %d\nFONT_DESCENT 0\nDEFAULT_CHAR 32\n" % TH)
    f.write("ENDPROPERTIES\n")
    f.write("CHARS %d\n" % len(codes))
    for cp in codes:
        w, rows = glyphs[cp]
        f.write("STARTCHAR U+%04X\n" % cp)
        f.write("ENCODING %d\n" % cp)
        f.write("SWIDTH %d 0\n" % int(w*1000/TH))
        f.write("DWIDTH %d 0\n" % w)
        f.write("BBX %d %d 0 0\n" % (w, TH))
        f.write("BITMAP\n")
        for r in rows:
            f.write(row_to_hex(r, w) + "\n")
        f.write("ENDCHAR\n")
    f.write("ENDFONT\n")

# ASCII preview
for cp in codes:
    w, rows = glyphs[cp]
    print("U+%04X '%s' w=%d" % (cp, chr(cp), w))
    for r in rows:
        print("".join("#" if v else "." for v in r))
    print()
print("wrote", OUT)
