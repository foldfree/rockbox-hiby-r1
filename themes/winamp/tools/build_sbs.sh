#!/bin/bash
# Build the Winamp base-skin (menu) backdrop: one window = groove titlebar + framed black list area.
set -e
SKIN=/tmp/winamp/skin
OUT=/tmp/winamptheme/wps/winamp/sbsbackdrop.bmp
T=/tmp/sbs; mkdir -p $T
FRAME='#333d4e'; HILITE='#626d82'; SHADOW='#151b26'; NAVY='#121a2e'

magick $SKIN/TITLEBAR.BMP -crop 1x14+80+0   +repage $T/slice.png
magick $SKIN/TITLEBAR.BMP -crop 52x14+250+0 +repage -filter point -resize 80x22\! $T/buttons.png
magick $SKIN/TITLEBAR.BMP -crop 24x14+27+0  +repage -filter point -resize 38x22\! $T/menu.png

# whole-screen window: steel frame + black interior (list area)
magick -size 480x800 xc:black \
  -fill "$FRAME"  -draw "roundrectangle 0,0 479,799 2,2" \
  -fill "$HILITE" -draw "line 0,0 479,0" -draw "line 0,0 0,799" \
  -fill "$SHADOW" -draw "line 479,0 479,799" -draw "line 0,799 479,799" \
  -fill black     -draw "rectangle 6,30 473,793" \
  $T/bd.png

# titlebar: groove + menu icon + buttons + a beveled inset navy panel for the title
magick $T/slice.png -filter point -resize 466x22\! $T/groove.png
magick $T/groove.png $T/menu.png -gravity West -composite $T/buttons.png -gravity East -composite $T/tb.png
magick $T/bd.png $T/tb.png -geometry +6+4 -composite \
  -fill "$NAVY"   -draw "rectangle 146,5 334,25" \
  -fill '#080c14' -draw "line 146,5 334,5"  -draw "line 146,5 146,25" \
  -fill '#3a4658' -draw "line 334,6 334,25" -draw "line 147,25 334,25" \
  $T/bd.png

magick $T/bd.png -type truecolor -compress none BMP3:$OUT
echo "sbs backdrop built -> $OUT"
