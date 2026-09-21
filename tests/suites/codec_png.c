/*
 * The PNG decoder: what comes out of a real file, and what a broken one gets instead.
 *
 * The decoder itself is Wuffs and is not this suite's subject - upstream tests it against a
 * corpus far larger than anything here, and the reason it was chosen is that it is memory-safe
 * by construction. What *is* ours is the configuration: which pixel format is asked for, which
 * sizes are refused, how the caller's two buffers are bounded, and which failures are told
 * apart. Every case below is about one of those.
 *
 * The colour case is the one worth reading twice. It checks the channel order against the
 * fixture's own palette rather than against values a working decoder produced, because a test
 * that asserts what the code already does cannot fail when the code changes - and a swizzle
 * handed the wrong format still decodes every file successfully, in the wrong colours, on a
 * screen nobody in CI is looking at.
 */

#include "framework/inkwell_test.h"

#include "inkwell/codec/png.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef INKWELL_TEST_DATA_DIR
#define INKWELL_TEST_DATA_DIR "tests/data"
#endif

#define PALETTE_PNG INKWELL_TEST_DATA_DIR "/png_palette_256.png"
#define TRUECOLOUR_PNG INKWELL_TEST_DATA_DIR "/png_truecolour_256.png"

/* Both fixtures are 256 square. The decoder takes the size as an argument, so this is the
   suite's fact about its files rather than anything the codec believes. */
#define FIXTURE_SIDE 256U
#define FIXTURE_PIXEL_BYTES ((size_t)FIXTURE_SIDE * FIXTURE_SIDE * INKWELL_PNG_PIXEL_BYTES)

/* Bigger than either fixture and smaller than the reader's own cap, so a test that grows a
   fixture finds out by failing to load it rather than by decoding half of one. */
#define FIXTURE_MAX (64U * 1024U)

/*
 * The caller's two buffers, which is what this API is about.
 *
 * Sized for the 4-bytes-a-pixel bracket, which is what inkwell/codec/png.h calls the sensible
 * default: it takes 32-bit RGBA and everything cheaper, and refuses sixteen bits a channel with
 * a stated -ENOTSUP. File scope rather than a case's stack because 256 KiB of scratch is not
 * something to put there, and because that is how a real caller declares them.
 */
static struct inkwell_png_decoder g_decoder;
static uint8_t g_work[262400U]; /* inkwell_png_workbuf_bytes(256, 256, 4) */

struct fixture {
    uint8_t *pixels;
    uint8_t encoded[FIXTURE_MAX];
    size_t encoded_len;
};

static size_t read_file(const char *path, uint8_t *out, size_t cap) {
    FILE *const file = fopen(path, "rb");
    if (file == NULL) {
        return 0U;
    }
    const size_t got = fread(out, 1U, cap, file);
    const bool whole = feof(file) != 0;
    fclose(file);
    return whole ? got : 0U;
}

static struct fixture *fixture_load(const char *path) {
    struct fixture *const fx = calloc(1U, sizeof *fx);
    if (fx == NULL) {
        return NULL;
    }
    fx->pixels = malloc(FIXTURE_PIXEL_BYTES);
    fx->encoded_len = read_file(path, fx->encoded, sizeof fx->encoded);
    if (fx->pixels == NULL || fx->encoded_len == 0U) {
        free(fx->pixels);
        free(fx);
        return NULL;
    }
    return fx;
}

static void fixture_free(struct fixture *fx) {
    if (fx != NULL) {
        free(fx->pixels);
        free(fx);
    }
}

/* The ordinary call every case below makes, so a case reads as the thing it is testing. */
static int decode(const uint8_t *encoded, size_t len, uint8_t *pixels, size_t pixels_len) {
    return inkwell_png_decode_bgra(&g_decoder, encoded, len, FIXTURE_SIDE, FIXTURE_SIDE, pixels,
                                   pixels_len, g_work, sizeof g_work);
}

/*
 * The fixture's own palette, read straight out of the file.
 *
 * Twenty lines of PNG chunk walking rather than a table of colours copied into this suite,
 * because the copied table is the thing being avoided: the claim under test is that the decoder
 * puts the file's colours in a stated order, and a claim checked against a constant somebody
 * typed is a claim about the typing.
 */
static size_t read_palette(const uint8_t *png, size_t len, uint8_t *out, size_t cap) {
    size_t at = 8U; /* past the signature */
    while (at + 8U <= len) {
        const uint32_t chunk = ((uint32_t)png[at] << 24) | ((uint32_t)png[at + 1U] << 16) |
                               ((uint32_t)png[at + 2U] << 8) | (uint32_t)png[at + 3U];
        if (memcmp(png + at + 4U, "PLTE", 4U) == 0) {
            if (chunk > cap || at + 8U + chunk > len) {
                return 0U;
            }
            memcpy(out, png + at + 8U, chunk);
            return chunk;
        }
        at += 12U + (size_t)chunk;
    }
    return 0U;
}

/*
 * A real file decodes to a full, opaque picture.
 *
 * "Opaque" is a claim about a compositing panel rather than about PNG: a display layer that
 * composites with per-pixel alpha draws a pixel whose alpha byte is zero as nothing at all, so
 * an image that decoded that way would be perfectly correct pixels that show nothing. That is
 * the failure worth a case, because it looks identical to the picture not being drawn.
 */
INKWELL_TEST_CASE(png_decodes_a_real_file, unit) {
    struct fixture *const fx = fixture_load(PALETTE_PNG);
    INKWELL_TEST_FAIL_IF(fx == NULL, "the palette fixture loads");

    const int decoded = decode(fx->encoded, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES);
    INKWELL_TEST_FAIL_IF_CLEANUP(decoded != 0, fixture_free(fx), "and decodes");

    size_t opaque = 0U;
    uint32_t distinct = 0U;
    uint32_t first = 0U;
    for (size_t i = 0U; i < FIXTURE_PIXEL_BYTES; i += INKWELL_PNG_PIXEL_BYTES) {
        if (fx->pixels[i + 3U] == 0xFFU) {
            ++opaque;
        }
        const uint32_t colour = (uint32_t)fx->pixels[i] | ((uint32_t)fx->pixels[i + 1U] << 8) |
                                ((uint32_t)fx->pixels[i + 2U] << 16);
        if (i == 0U) {
            first = colour;
        } else if (colour != first) {
            distinct = 1U;
        }
    }
    INKWELL_TEST_FAIL_IF_CLEANUP(opaque != (size_t)FIXTURE_SIDE * FIXTURE_SIDE, fixture_free(fx),
                                 "every pixel is opaque");
    /* And it is a picture rather than a fill - a decode that wrote one colour over the whole
       buffer would pass every other assertion here. */
    INKWELL_TEST_FAIL_IF_CLEANUP(distinct == 0U, fixture_free(fx), "and is not one flat colour");
    fixture_free(fx);
    record_success(test_name);
}

/*
 * The channel order, checked against the file rather than against ourselves.
 *
 * Every pixel of an indexed PNG is by definition one of its palette entries, so every decoded
 * pixel's red, green and blue - read out of the byte positions inkwell/codec/png.h promises them
 * in - has to appear in the PLTE chunk. What makes this a real check rather than a tautology is
 * a property of the fixture, recorded in tests/data/README.md: not one of its 31 entries has its
 * red and blue also present swapped. So a decoder configured for RGBA where the header promised
 * BGRA fails this on every non-grey pixel in the picture, which is most of them - where a test
 * asserting "the middle pixel is #71787F" would simply be re-recorded by whoever broke it.
 */
INKWELL_TEST_CASE(png_colours_come_from_the_files_own_palette, unit) {
    struct fixture *const fx = fixture_load(PALETTE_PNG);
    INKWELL_TEST_FAIL_IF(fx == NULL, "the palette fixture loads");

    uint8_t palette[256U * 3U];
    const size_t palette_len = read_palette(fx->encoded, fx->encoded_len, palette, sizeof palette);
    INKWELL_TEST_FAIL_IF_CLEANUP(palette_len == 0U || palette_len % 3U != 0U, fixture_free(fx),
                                 "the fixture carries a palette");

    INKWELL_TEST_FAIL_IF_CLEANUP(
        decode(fx->encoded, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES) != 0,
        fixture_free(fx), "and decodes");

    /* The property the check rests on, asserted here rather than trusted from the README: a
       fixture regenerated one day into a palette that is symmetric under a red/blue swap would
       leave this case passing while testing nothing. */
    size_t symmetric = 0U;
    for (size_t i = 0U; i < palette_len; i += 3U) {
        if (palette[i] == palette[i + 2U]) {
            continue; /* a grey, which says nothing either way */
        }
        for (size_t j = 0U; j < palette_len; j += 3U) {
            if (palette[j] == palette[i + 2U] && palette[j + 1U] == palette[i + 1U] &&
                palette[j + 2U] == palette[i]) {
                ++symmetric;
                break;
            }
        }
    }
    INKWELL_TEST_FAIL_IF_CLEANUP(symmetric != 0U, fixture_free(fx),
                                 "and no entry of it is its own red/blue swap");

    for (size_t i = 0U; i < FIXTURE_PIXEL_BYTES; i += INKWELL_PNG_PIXEL_BYTES) {
        const uint8_t blue = fx->pixels[i];
        const uint8_t green = fx->pixels[i + 1U];
        const uint8_t red = fx->pixels[i + 2U];
        bool found = false;
        for (size_t entry = 0U; entry < palette_len && !found; entry += 3U) {
            found = palette[entry] == red && palette[entry + 1U] == green &&
                    palette[entry + 2U] == blue;
        }
        INKWELL_TEST_FAIL_IF_CLEANUP(!found, fixture_free(fx),
                                     "every decoded pixel is one of the file's own colours");
    }
    fixture_free(fx);
    record_success(test_name);
}

/*
 * The other colour type, which reaches the swizzler by a different path.
 *
 * A palette PNG and a 24-bit one are handled differently inside the decoder - one through a
 * palette lookup, one straight - so an application whose producer stopped quantising would
 * switch onto the second without anything else changing.
 */
INKWELL_TEST_CASE(png_decodes_a_truecolour_file, unit) {
    struct fixture *const fx = fixture_load(TRUECOLOUR_PNG);
    INKWELL_TEST_FAIL_IF(fx == NULL, "the truecolour fixture loads");

    INKWELL_TEST_FAIL_IF_CLEANUP(
        decode(fx->encoded, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES) != 0,
        fixture_free(fx), "a 24-bit file decodes too");
    for (size_t i = 0U; i < FIXTURE_PIXEL_BYTES; i += INKWELL_PNG_PIXEL_BYTES) {
        INKWELL_TEST_FAIL_IF_CLEANUP(fx->pixels[i + 3U] != 0xFFU, fixture_free(fx),
                                     "and is opaque throughout, having carried no alpha at all");
    }
    fixture_free(fx);
    record_success(test_name);
}

/*
 * Everything that is not a decodable file, and the different answers each gets.
 *
 * -EILSEQ and -ENOBUFS are kept apart on purpose: one is a file being broken and the other is
 * the caller asking wrong. Image bytes come off storage somebody can unplug, so the first is a
 * Tuesday and the second is a bug, and a caller that could not tell them apart would either log
 * a bug report for a scratched card or swallow its own mistake as bad input.
 */
INKWELL_TEST_CASE(png_refuses_what_is_not_an_image, unit) {
    struct fixture *const fx = fixture_load(PALETTE_PNG);
    INKWELL_TEST_FAIL_IF(fx == NULL, "the palette fixture loads");

    INKWELL_TEST_FAIL_IF_CLEANUP(decode(NULL, 10U, fx->pixels, FIXTURE_PIXEL_BYTES) != -EINVAL,
                                 fixture_free(fx), "no bytes is -EINVAL");
    INKWELL_TEST_FAIL_IF_CLEANUP(decode(fx->encoded, 0U, fx->pixels, FIXTURE_PIXEL_BYTES) !=
                                     -EINVAL,
                                 fixture_free(fx), "and so is a length of nothing");
    INKWELL_TEST_FAIL_IF_CLEANUP(decode(fx->encoded, fx->encoded_len, NULL, FIXTURE_PIXEL_BYTES) !=
                                     -EINVAL,
                                 fixture_free(fx), "and so is nowhere to put it");
    INKWELL_TEST_FAIL_IF_CLEANUP(
        inkwell_png_decode_bgra(NULL, fx->encoded, fx->encoded_len, FIXTURE_SIDE, FIXTURE_SIDE,
                                fx->pixels, FIXTURE_PIXEL_BYTES, g_work, sizeof g_work) != -EINVAL,
        fixture_free(fx), "and so is no decoder state");

    /* A buffer one byte short. Refused rather than filled as far as it goes, because a partly
       written image is a picture with a torn edge and nothing downstream could tell. */
    INKWELL_TEST_FAIL_IF_CLEANUP(
        decode(fx->encoded, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES - 1U) != -ENOBUFS,
        fixture_free(fx), "a buffer short of the image is -ENOBUFS");

    /* A real PNG, asked for at a size it is not. Refused rather than scaled or cropped: a
       caller that budgeted a fixed destination has no use for a different picture in it. */
    INKWELL_TEST_FAIL_IF_CLEANUP(
        inkwell_png_decode_bgra(&g_decoder, fx->encoded, fx->encoded_len, 128U, 128U, fx->pixels,
                                FIXTURE_PIXEL_BYTES, g_work, sizeof g_work) != -EINVAL,
        fixture_free(fx), "an image of another size is -EINVAL");

    /* Not a PNG at all. */
    static const uint8_t k_not_png[] = "PACK2 and then some bytes that are not an image";
    INKWELL_TEST_FAIL_IF_CLEANUP(
        decode(k_not_png, sizeof k_not_png, fx->pixels, FIXTURE_PIXEL_BYTES) != -EILSEQ,
        fixture_free(fx), "bytes that are not a PNG are -EILSEQ");

    /*
     * A file cut short - the shape a copy interrupted half way produces, and the one a
     * streaming decoder gets wrong by waiting politely for the rest of a file that has no rest.
     */
    INKWELL_TEST_FAIL_IF_CLEANUP(
        decode(fx->encoded, fx->encoded_len / 2U, fx->pixels, FIXTURE_PIXEL_BYTES) != -EILSEQ,
        fixture_free(fx), "half a file is -EILSEQ rather than a wait");

    /* And one with its image data corrupted but its length intact, which is what a bad sector
       under a file that was never re-read looks like. The header still parses; the zlib stream
       under it does not. */
    uint8_t *const damaged = malloc(fx->encoded_len);
    INKWELL_TEST_FAIL_IF_CLEANUP(damaged == NULL, fixture_free(fx), "a copy to damage");
    memcpy(damaged, fx->encoded, fx->encoded_len);
    for (size_t i = fx->encoded_len / 2U; i < fx->encoded_len / 2U + 64U; ++i) {
        damaged[i] = (uint8_t)~damaged[i];
    }
    const int hurt = decode(damaged, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES);
    free(damaged);
    INKWELL_TEST_FAIL_IF_CLEANUP(hurt != -EILSEQ, fixture_free(fx),
                                 "and damaged image data is -EILSEQ, not a picture of the damage");
    fixture_free(fx);
    record_success(test_name);
}

/*
 * A scratch buffer too small for the file is -ENOTSUP, and the sizing function says how small.
 *
 * This is the half of the contract the caller owns, and the one that was a private constant
 * before this was a library: a caller picks the colour-depth bracket it will pay for and a file
 * above it is refused in a way it can report. -ENOTSUP rather than -ENOBUFS because nothing the
 * caller did is wrong - it budgeted what it meant to - and because it doubles as the guard on a
 * Wuffs whose work-buffer arithmetic moved, which would otherwise stop decoding for a reason
 * nothing could print.
 */
INKWELL_TEST_CASE(png_refuses_a_file_deeper_than_its_scratch, unit) {
    /* width * bytes_per_pixel * height + width, which is what the header states. */
    INKWELL_TEST_FAIL_IF(inkwell_png_workbuf_bytes(256U, 256U, 1U) != 65792U ||
                             inkwell_png_workbuf_bytes(256U, 256U, 3U) != 196864U ||
                             inkwell_png_workbuf_bytes(256U, 256U, 4U) != 262400U ||
                             inkwell_png_workbuf_bytes(256U, 256U, 8U) != 524544U,
                         "the work buffer is width * bpp * height + width");
    INKWELL_TEST_FAIL_IF(inkwell_png_workbuf_bytes(0U, 256U, 4U) != 0U ||
                             inkwell_png_workbuf_bytes(256U, 0U, 4U) != 0U ||
                             inkwell_png_workbuf_bytes(256U, 256U, 0U) != 0U,
                         "and a size nothing could decode needs no buffer");

    struct fixture *const fx = fixture_load(TRUECOLOUR_PNG);
    INKWELL_TEST_FAIL_IF(fx == NULL, "the truecolour fixture loads");

    /* Budgeted for one byte a pixel, handed a 24-bit file. The palette bracket is a real choice
       an application might make - it is the cheapest - so this is the refusal it would meet. */
    static uint8_t narrow_work[65792U]; /* inkwell_png_workbuf_bytes(256, 256, 1) */
    INKWELL_TEST_FAIL_IF_CLEANUP(inkwell_png_decode_bgra(&g_decoder, fx->encoded, fx->encoded_len,
                                                         FIXTURE_SIDE, FIXTURE_SIDE, fx->pixels,
                                                         FIXTURE_PIXEL_BYTES, narrow_work,
                                                         sizeof narrow_work) != -ENOTSUP,
                                 fixture_free(fx), "a file deeper than the scratch is -ENOTSUP");

    /* And the same file with the buffer it actually needs. The pair is the point: without it
       the case above would pass for a decoder that refused everything. */
    INKWELL_TEST_FAIL_IF_CLEANUP(
        decode(fx->encoded, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES) != 0,
        fixture_free(fx), "and decodes once the scratch is the size it asked for");
    fixture_free(fx);
    record_success(test_name);
}

/*
 * A file damaged after its last pixel still decodes, and one damaged before it does not.
 *
 * The pair is the point. A cut that reaches the pixel data fails, because the zlib stream
 * carries its own checksum and a short one cannot be read out; a cut that only reaches IEND -
 * twelve bytes of constant, carrying nothing - leaves a picture that is whole and verified, and
 * refusing it would put a blank square where a correct image exists. There is no case where
 * enforcing the terminator stops a *wrong* picture from being drawn.
 *
 * Written down as a case because it is a decision, and an undecided one looks identical: the
 * decoder does this whether or not anybody meant it to, so without this the next person to read
 * the contract would have to guess which it was.
 */
INKWELL_TEST_CASE(png_reads_a_file_damaged_after_its_last_pixel, unit) {
    struct fixture *const fx = fixture_load(PALETTE_PNG);
    INKWELL_TEST_FAIL_IF(fx == NULL, "the palette fixture loads");

    /* Where IEND starts: the last chunk, twelve bytes of length, type and checksum. */
    const size_t terminator = fx->encoded_len - 12U;
    INKWELL_TEST_FAIL_IF_CLEANUP(memcmp(fx->encoded + terminator + 4U, "IEND", 4U) != 0,
                                 fixture_free(fx), "the fixture ends with an IEND chunk");

    /* Gone entirely, and the pixels still come out. */
    INKWELL_TEST_FAIL_IF_CLEANUP(decode(fx->encoded, terminator, fx->pixels, FIXTURE_PIXEL_BYTES) !=
                                     0,
                                 fixture_free(fx), "a file with no terminator still decodes");

    /* And present but wrong, which is what a bad sector on those twelve bytes looks like. */
    uint8_t *const damaged = malloc(fx->encoded_len);
    INKWELL_TEST_FAIL_IF_CLEANUP(damaged == NULL, fixture_free(fx), "a copy to damage");
    memcpy(damaged, fx->encoded, fx->encoded_len);
    damaged[fx->encoded_len - 1U] = (uint8_t)~damaged[fx->encoded_len - 1U];
    const int tail = decode(damaged, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES);
    free(damaged);
    INKWELL_TEST_FAIL_IF_CLEANUP(tail != 0, fixture_free(fx),
                                 "and so does one whose terminator is corrupt");

    /*
     * The other side of the line: twenty bytes short reaches into the compressed pixels, and
     * that is refused. Without this half the case above would read as "the decoder ignores
     * damage", which is the opposite of what it does.
     */
    INKWELL_TEST_FAIL_IF_CLEANUP(
        decode(fx->encoded, fx->encoded_len - 20U, fx->pixels, FIXTURE_PIXEL_BYTES) != -EILSEQ,
        fixture_free(fx), "a cut that reaches the pixels is -EILSEQ");
    fixture_free(fx);
    record_success(test_name);
}

/*
 * Decoding holds a constant amount of memory, and decoding more files does not hold more.
 *
 * The promise a decoder had to prove, and the reason this one was picked over an allocating
 * library: a decode on an event loop cannot pause to find memory and cannot fail for want of it.
 * Here the buffers are the caller's, so what has to hold is that ten decodes want no more than
 * the first did - state that grows per call is invisible in a single one - and that the state
 * block a caller declares is a stated, bounded size rather than whatever upstream last needed.
 */
INKWELL_TEST_CASE(png_holds_a_bounded_amount_of_memory, unit) {
    /* A bound rather than the figure, because the figure is upstream's to move a little - but a
       *large* move is a caller's memory budget being quietly rewritten by a dependency, which is
       worth failing over. 48 KiB of state against the 44,632 bytes the pinned version asks. */
    INKWELL_TEST_FAIL_IF(sizeof(struct inkwell_png_decoder) != INKWELL_PNG_DECODER_BYTES ||
                             INKWELL_PNG_DECODER_BYTES > 64U * 1024U,
                         "the decoder's state is stated and bounded");

    struct fixture *const fx = fixture_load(PALETTE_PNG);
    INKWELL_TEST_FAIL_IF(fx == NULL, "the palette fixture loads");
    for (unsigned round = 0U; round < 10U; ++round) {
        INKWELL_TEST_FAIL_IF_CLEANUP(
            decode(fx->encoded, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES) != 0,
            fixture_free(fx), "a file decodes every time it is asked");
    }

    /* A failed decode has to leave the next one working. The decoder is re-initialised per
       call for exactly this reason, and a state machine left mid-image by a broken file is the
       ordinary way that stops being true. */
    static const uint8_t k_rubbish[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    (void)decode(k_rubbish, sizeof k_rubbish, fx->pixels, FIXTURE_PIXEL_BYTES);
    INKWELL_TEST_FAIL_IF_CLEANUP(
        decode(fx->encoded, fx->encoded_len, fx->pixels, FIXTURE_PIXEL_BYTES) != 0,
        fixture_free(fx), "and a broken file does not poison the next one");
    fixture_free(fx);
    record_success(test_name);
}
