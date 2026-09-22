# Test fixtures

A parser that reads bytes somebody else wrote is only really tested by bytes somebody else
wrote. These are real captures rather than documents written to suit the parser, which is the
whole reason they are files here instead of string literals in a suite.

`tests/CMakeLists.txt` points `INKWELL_TEST_DATA_DIR` at this *source* directory, so nothing is
copied at configure time and a fixture refreshed on disk is the one the next run reads.

| File | What it is | Staged? |
|---|---|---|
| `zip_tail_nrf52840_2.7.26.bin` | the last 64 KiB of a real release zip - its central directory and end record | the tail only, not the archive |
| `zip_tail_nrf52840_2.8.0.bin` | the same, from a later release whose directory is laid out differently | the tail only |
| `zip_member_t114_mt_json_2.7.26.bin` | one member of a release zip, local header and deflated data, exactly as served | nothing - all of it |
| `t114_2.7.26.uf2` | first two and last two 512-byte blocks of a real 2.7.26 nRF52840 image | four of 2,866 blocks |
| `png_palette_256.png` | a 256-square 8-bit indexed PNG | nothing - all 9,391 bytes |
| `png_truecolour_256.png` | a 256-square 24-bit RGB PNG | nothing - all 25,868 bytes |

## The two PNGs

They are the two colour types that reach the swizzler by different paths inside the decoder -
one through a palette lookup, one straight - so a suite that used only the first would not
notice the second breaking. Each is the median tile by size of the set it came from rather than
a hand-picked one, so neither is easy by selection.

`png_palette_256.png` carries the property the colour test rests on: of its 31 palette entries,
**not one has its red and blue swapped also present**. That is what lets `suites/codec_png.c`
check the decoder's channel order against the file rather than against itself - every decoded
pixel's RGB has to appear in the PNG's own `PLTE` chunk, which a swizzle that swapped two
channels fails on every non-grey pixel in the picture. The alternative was asserting the colours
a working decoder happened to produce, which is the fixture-tests-the-parser-against-itself trap
one level down. `codec_png.c` re-derives the property from the file rather than trusting this
paragraph, so a fixture regenerated into a symmetric palette fails rather than quietly testing
nothing.

## The zip member

`zip_member_t114_mt_json_2.7.26.bin` is what `suites/codec_inflate.c` inflates: a 30-byte local
header, a 53-byte name, a 28-byte extra field, and then the deflated data. The length and CRC it
must come out to are the ones that archive's central directory carries for it, which is the
pairing `codec/zip.h` leaves to `codec/inflate.h` - so the case checks both halves of a real
archive against each other rather than against a number typed into a test.

## The UF2 image

`t114_2.7.26.uf2` preserves blocks 0, 1, 2864 and 2865 of the captured image.
The first two form a truncated file; all four form a broken sequence. The codec suite
rewrites only the `numBlocks` field when it needs a complete one- or two-block image.
The addresses, family and payload sizes remain the bytes published in the release.
