#!/bin/bash
# Rebuild the Winamp backdrop with three windows, each with a real Winamp titlebar.
set -e
SKIN=/tmp/winamp/skin
SRC=/tmp/winamptheme/wps/winamp/backdrop.base.bmp   # main window only (saved clean)
OUT=/tmp/winamptheme/wps/winamp/backdrop.bmp
T=/tmp/bd; mkdir -p $T

FRAME='#333d4e'; HILITE='#626d82'; SHADOW='#151b26'; NAVY='#121a2e'

# titlebar building blocks from TITLEBAR.BMP (active bar = top row, 14px)
magick $SKIN/TITLEBAR.BMP -crop 1x14+80+0   +repage $T/slice.png
magick $SKIN/TITLEBAR.BMP -crop 32x12+270+1 +repage -filter point -resize 48x20\! $T/buttons.png
magick $SKIN/TITLEBAR.BMP -crop 22x12+27+1  +repage -filter point -resize 34x20\! $T/menu.png

# groove bar of width $1 -> $T/groove.png
groove(){ magick $T/slice.png -filter point -resize ${1}x22\! $T/groove.png; }

# titlebar(width, with_menu) -> $T/tb.png  (groove + buttons [+ menu])
titlebar(){ groove $1
  if [ "$2" = menu ]; then
     magick $T/groove.png $T/menu.png -gravity West -composite $T/buttons.png -gravity East -composite $T/tb.png
  else
     magick $T/groove.png $T/buttons.png -gravity East -composite $T/tb.png
  fi; }

cp "$SRC" $T/bd.png

# --- MAIN window: full-width groove (clean corners) + inset menu icon & buttons ---
groove 473
magick $T/groove.png \
       $T/menu.png    -geometry +6+1   -composite \
       $T/buttons.png -geometry +421+1 -composite \
       $T/tb.png
magick $T/bd.png $T/tb.png -geometry +3+5 -composite \
       -fill "$NAVY" -draw "rectangle 206,7 274,25" $T/bd.png   # navy plate for WINAMP text

# --- wipe everything below the main window, draw 2 fresh windows ---
magick $T/bd.png -fill black -draw "rectangle 0,208 479,799" $T/bd.png

win(){  # x0 y0 x1 y1
  magick $T/bd.png \
    -fill "$FRAME"  -draw "roundrectangle $1,$2 $3,$4 2,2" \
    -fill "$HILITE" -draw "line $1,$2 $3,$2" -draw "line $1,$2 $1,$4" \
    -fill "$SHADOW" -draw "line $3,$2 $3,$4" -draw "line $1,$4 $3,$4" \
    -fill black     -draw "rectangle $(($1+6)),$(($2+6)) $(($3-6)),$(($4-6))" $T/bd.png; }

# --- WINAMP PLAYLIST window: full width y210..304 ---
win 0 210 479 304
titlebar 466
magick $T/bd.png $T/tb.png -geometry +6+216 -composite \
       -fill "$NAVY" -draw "rectangle 162,218 318,236" \
       -fill '#0000c6' -draw "rectangle 7,242 472,266" $T/bd.png   # blue 'now playing' row

# --- ALBUM ART window: full width y312..793 ---
win 0 312 479 793
titlebar 466
magick $T/bd.png $T/tb.png -geometry +6+318 -composite \
       -fill "$NAVY" -draw "rectangle 194,320 286,338" $T/bd.png

magick $T/bd.png -type truecolor -compress none BMP3:$OUT
echo "backdrop rebuilt -> $OUT"
