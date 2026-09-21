#include "inkwell/codec/png.h"

#include "inkwell_wuffs.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/*
 * The PNG decoder. One of the two files here that include Wuffs; src/codec/inflate.c is the
 * other.
 *
 * Every buffer is the caller's, for the reasons inkwell/codec/png.h gives. What is left in this
 * file is the configuration - which pixel format is asked for, which sizes are refused, and
 * which failures are told apart - and that configuration is what codec_png.c tests.
 */

size_t inkwell_png_workbuf_bytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel) {
    if (width == 0U || height == 0U || bytes_per_pixel == 0U) {
        return 0U;
    }
    /* width * bytes_per_pixel * height + width, in 64 bits throughout so an overflow is a
       refusal rather than a small number that reads as a valid budget. */
    const uint64_t row = (uint64_t)width * (uint64_t)bytes_per_pixel;
    const uint64_t total = row * (uint64_t)height + (uint64_t)width;
    if (total > (uint64_t)SIZE_MAX) {
        return 0U;
    }
    return (size_t)total;
}

int inkwell_png_decode_bgra(struct inkwell_png_decoder *decoder_state, const uint8_t *encoded,
                            size_t len, uint32_t width, uint32_t height, uint8_t *pixels,
                            size_t pixels_len, uint8_t *work, size_t work_len) {
    if (decoder_state == NULL || encoded == NULL || len == 0U || pixels == NULL || width == 0U ||
        height == 0U || (work == NULL && work_len != 0U)) {
        return -EINVAL;
    }
    const size_t wanted_pixels = (size_t)width * (size_t)height * (size_t)INKWELL_PNG_PIXEL_BYTES;
    if (pixels_len < wanted_pixels) {
        return -ENOBUFS;
    }

    /* The decoder lives in the caller's bytes rather than in a struct of ours, because the
       struct is opaque in C by upstream's choice - see INKWELL_PNG_DECODER_BYTES. Its real size
       has to be passed as it is rather than as the block's, or Wuffs refuses the receiver as the
       wrong shape. */
    if (sizeof__wuffs_png__decoder() > sizeof decoder_state->opaque) {
        return -ENOBUFS;
    }
    wuffs_png__decoder *const decoder = (wuffs_png__decoder *)(void *)decoder_state->opaque;
    wuffs_base__status status =
        wuffs_png__decoder__initialize(decoder, sizeof__wuffs_png__decoder(), WUFFS_VERSION, 0);
    if (!wuffs_base__status__is_ok(&status)) {
        return -EINVAL;
    }

    /*
     * Wuffs reads through a cursor over a buffer it does not own, and `true` is the promise that
     * these bytes are the whole file rather than the start of a stream. It is what turns a
     * truncated image into an error here instead of a request for more that nobody can answer -
     * the decoder would otherwise sit waiting on the rest of a file that has no rest.
     */
    wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader((uint8_t *)encoded, len, true);

    wuffs_base__image_config config;
    status = wuffs_png__decoder__decode_image_config(decoder, &config, &source);
    if (!wuffs_base__status__is_ok(&status)) {
        return -EILSEQ;
    }
    if (wuffs_base__pixel_config__width(&config.pixcfg) != width ||
        wuffs_base__pixel_config__height(&config.pixcfg) != height) {
        return -EINVAL;
    }

    wuffs_base__pixel_config__set(&config.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    wuffs_base__pixel_buffer destination;
    status = wuffs_base__pixel_buffer__set_from_slice(
        &destination, &config.pixcfg, wuffs_base__make_slice_u8(pixels, wanted_pixels));
    if (!wuffs_base__status__is_ok(&status)) {
        return -ENOBUFS;
    }

    /* A file whose pixels need more scratch than the caller budgeted. See -ENOTSUP in the
       header for why it is not -ENOBUFS. */
    const wuffs_base__range_ii_u64 wanted = wuffs_png__decoder__workbuf_len(decoder);
    if (wanted.min_incl > (uint64_t)work_len) {
        return -ENOTSUP;
    }

    status = wuffs_png__decoder__decode_frame(decoder, &destination, &source,
                                              WUFFS_BASE__PIXEL_BLEND__SRC,
                                              wuffs_base__make_slice_u8(work, work_len), NULL);
    if (!wuffs_base__status__is_ok(&status)) {
        return -EILSEQ;
    }
    return 0;
}
