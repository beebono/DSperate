# DSperate's DSi system font

`TWLFontTable.dat` stands in for the DSi's `/sys/TWLFontTable.dat` on a
NAND DSperate makes up (`io/dsi_nand_synth`). It is generated, not dumped:

    tools/make_dsi_font.py NotoSans-Regular.ttf wqy-microhei.ttc TWLFontTable.dat

from these files (regenerate only with the same ones, or record the new ones
here):

| face | file | from | SHA-256 |
|---|---|---|---|
| Noto Sans Regular | NotoSans-Regular.ttf | Debian/Ubuntu `fonts-noto-core` 20201225-2 | `89c3c497f618fdaa0b2d1e98fef93582f28c71debd2c4a8cdf41f190ced2909d` |
| WenQuanYi Micro Hei | wqy-microhei.ttc | Debian/Ubuntu `fonts-wqy-microhei` 0.2.0-beta-3.1 | `2420e8078af796b19a3f6ef13de527a1a91c1e7171eea115926c614ced1009b3` |

Output SHA-256: `45045fba53c9f9e069abb9c229cfc23869da02463c30a93874a18af1c71f553c`.

Three Nitro fonts (16x21, 12x16, 10x12 cells, 2 bpp), 7372 characters each:
Latin-1 and Latin Extended-A, Greek, Cyrillic and common symbols from Noto
Sans; kana, JIS X 0208 kanji and full-width forms from WenQuanYi Micro Hei.
Nintendo's private-use button symbols (U+E000-U+E06B) are absent: text using
them draws the default character.

The table carries a plain-text marker where Nintendo's RSA signature goes.
Titles check that signature through the DSi BIOS (SWI 22h); DSperate answers
that call for this marker with the header's SHA-1 (`NDS::dsi_hle_swi`). Every
hash inside the file is genuine.

Licences: `LICENSE-NotoSans-OFL-1.1.txt` (the Noto-derived glyphs are OFL
1.1) and `LICENSE-WenQuanYi-MicroHei.txt` (GPL-3+ with the font exception).
