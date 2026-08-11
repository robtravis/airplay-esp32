# Silkscreen — screen typeface

Pixel font from Google Fonts, converted for LVGL. Chosen for the retro character
the radio firmware had from TFT_eSPI's built-in font, without unscii's heavy
strokes.

* Upstream: https://github.com/googlefonts/silkscreen
* Licence: SIL Open Font License 1.1 — embedding and redistribution are
  explicitly permitted, which matters because these are gift units. (Monaco and
  Menlo would have been convenient and are not redistributable.)

Regenerate with:

```bash
npx lv_font_conv --font Silkscreen-Regular.ttf --size 16 --bpp 1 --format lvgl \
  --range 0x20-0x7F --lv-include lvgl.h -o silkscreen_16.c
npx lv_font_conv --font Silkscreen-Regular.ttf --size 32 --bpp 1 --format lvgl \
  --range 0x20-0x7F --lv-include lvgl.h -o silkscreen_32.c
```

`--bpp 1` is deliberate: a pixel font wants hard on/off pixels, and antialiasing
muddies it. The range is ASCII only — LVGL's symbol glyphs (the lightning bolt)
come from the montserrat fonts, so the bolt label keeps that font.

Silkscreen is designed on an 8px grid, so 16 and 32 land on exact multiples and
stay crisp. Intermediate sizes will not.


## Bitcount Single — the active face

Dot-matrix variable font, also OFL. Narrower than Silkscreen (a long track title
fits at 32px instead of scrolling) and it has real lowercase.

```bash
npx lv_font_conv --font BitcountSingle-VF.ttf --size 32 --bpp 4 --format lvgl \
  --range 0x20-0x7F --lv-include lvgl.h --no-compress -o bitcount_32.c
```

**`--no-compress` is mandatory, not a preference.** lv_font_conv compresses glyph
bitmaps by default whenever `--bpp` is greater than 1, which sets
`.bitmap_format = 1` in the generated file. LVGL then needs
`CONFIG_LV_USE_FONT_COMPRESSED=y` to decode it, and without that it draws
**nothing at all** — no error, no warning, no missing-glyph boxes, just blank
text. That is exactly what happened on the first Bitcount flash, and it is
invisible in the generated C unless you look for `.bitmap_format`.

Silkscreen never hit this because it was converted at `--bpp 1`, where no
compression is applied.

The alternative fix — enabling LV_USE_FONT_COMPRESSED — was rejected: it costs
glyph decoding on every draw, and this firmware's whole failure mode is stealing
CPU from the audio pump.

`--bpp 4` is deliberate: Bitcount's elements are round dots, and 1bpp squares
them off into the blockiness the face exists to avoid.
