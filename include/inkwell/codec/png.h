#pragma once

/*
 * A PNG turned into pixels, in a stated channel order, with no allocation anywhere.
 *
 * It is a seam rather than a wrapper. Nothing above it names Wuffs, includes its header or knows
 * a PNG from a JPEG - what this promises is a block of pixels in a stated order, which is the
 * only thing a cache or a blit has any business relying on. The decoder behind it is memory-safe
 * by construction, which matters more than anything else for a decoder whose input is a file the
 * application did not write; see third_party/wuffs/README.md for why it is the one here.
 *
 * ---- who owns the memory ---------------------------------------------------------------------
 *
 * The caller does, all of it. Wuffs allocates nothing and has no opinion about where its state
 * lives: it is handed a struct and a scratch buffer and uses those and nothing else, so the
 * whole of what a decode costs is what the caller passed in. That is a constant rather than a
 * total, which is the property that matters on an event loop - a decode that cannot ask for
 * memory cannot fail for want of it and cannot stop to look.
 *
 * Two blocks, because they are sized by different things. The decoder's own state is a fixed
 * cost (INKWELL_PNG_DECODER_BYTES) and can be a file-scope object that is never touched until
 * the first decode. The work buffer scales with the image and with its *colour type*, so only
 * the caller - who knows what it is about to decode and what it is willing to spend - can size
 * it. inkwell_png_workbuf_bytes() answers what a given size costs at a given depth.
 *
 * The obvious API, where this allocates both and returns a handle, was not taken. On a
 * single-threaded loop there is exactly one decode in flight ever, so a second handle is not
 * something anything could use, and a lazy allocation adds a failure path on the one call where
 * there is nothing sensible to do about it. Static storage is zero-page-backed until it is
 * touched, so an application that never decodes a PNG pays address space rather than memory.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Four bytes a pixel, in the order **blue, green, red, alpha**.
 *
 * Stated in bytes rather than as a word, because a word is a claim about the host's endianness
 * and this is a claim about memory. On a little-endian host, reading four of them as a uint32_t
 * gives 0xAARRGGBB, which is what most framebuffer panels want: a compositor with per-pixel
 * alpha finds the alpha byte load-bearing rather than padding, and a pixel that arrives
 * 0x00RRGGBB is invisible rather than black. An opaque PNG decodes with alpha 255 throughout.
 *
 * A panel's real channel layout is a runtime fact and a blit converts when it has to. What this
 * fixes is that there is one format to convert *from*, decided once, instead of a decoder
 * handing back whatever the file happened to contain.
 */
#define INKWELL_PNG_PIXEL_BYTES 4U

/*
 * How much storage `struct inkwell_png_decoder` reserves for Wuffs' own state.
 *
 * Wuffs reports its struct's size rather than documenting it - upstream says in as many words
 * that its fields and its size "aren't guaranteed to be stable across Wuffs versions" - so this
 * is a bound checked at every decode rather than a number anyone may rely on. 48 KiB against the
 * 44,632 bytes the pinned version asks for is enough headroom for one that grows a little and
 * not enough to hide one that grows a lot, which is what the check is for.
 */
#define INKWELL_PNG_DECODER_BYTES (48U * 1024U)

/*
 * Where a decode keeps its state. Opaque: nothing outside codec/png.c may read a field of it,
 * and the layout is Wuffs' rather than ours.
 *
 * It is a struct rather than a bare array so that a caller can declare one at file scope, in a
 * thread-local, or inside something larger, without having to know the alignment - which is
 * max_align_t, and which a bare uint8_t array would get wrong silently.
 */
struct inkwell_png_decoder {
    _Alignas(max_align_t) uint8_t opaque[INKWELL_PNG_DECODER_BYTES];
};

/*
 * How many scratch bytes decoding a `width` x `height` image needs, at `bytes_per_pixel` of
 * *source* colour depth - which is the file's colour type, not the BGRA this hands back.
 *
 * Wuffs wants `width * bytes_per_pixel * height + width`. Measured on the pinned version at 256
 * px square:
 *
 *     8-bit grey, 8-bit palette   1 byte a pixel     65,792
 *     24-bit RGB                  3 bytes a pixel   196,864
 *     32-bit RGBA                 4 bytes a pixel   262,400
 *     16-bit-per-channel RGBA     8 bytes a pixel   524,544
 *
 * A caller picks the bracket it is willing to pay for and gets INKWELL_PNG_TOO_DEEP for a file
 * above it, which is a stated refusal rather than a crash or a wrong picture. Sizing for 4 is
 * the sensible default: a style with transparency in it is an ordinary thing to publish, and 8
 * is twice the memory for precision no panel of this kind can show and no ordinary encoder
 * emits.
 *
 * Returns 0 for arguments that would overflow, which a caller must treat as "cannot decode
 * that" rather than as "needs no buffer".
 */
size_t inkwell_png_workbuf_bytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel);

/*
 * Decodes `encoded` into `pixels` as INKWELL_PNG_PIXEL_BYTES-per-pixel BGRA.
 *
 * The image must be exactly `width` x `height`; a PNG of another size is refused rather than
 * scaled, because a caller that budgeted a fixed destination has no use for a different one and
 * silently cropping would be worse than saying no. `pixels` must hold at least
 * `width * height * INKWELL_PNG_PIXEL_BYTES` bytes and all of it is written. `work` is scratch,
 * sized with inkwell_png_workbuf_bytes(); it is read and written freely and holds nothing
 * between calls.
 *
 * Asking for BGRA is what makes the conversion free rather than a second pass. A palette PNG's
 * own pixels are indices, so the decoder has to walk them through a palette into *something*;
 * asking for the order the caller wants costs the same pass as asking for the file's.
 *
 * 0 on success. Otherwise:
 *
 *   -EINVAL   the arguments are not usable, or the image is not `width` x `height`
 *   -ENOBUFS  `pixels` is too small, or the decoder's own state outgrew this struct
 *   -EILSEQ   the bytes are not a PNG, or its pixels cannot be read out whole
 *   -ENOTSUP  a PNG whose pixels need more scratch than `work` holds - sixteen bits a channel,
 *             in practice. Not -ENOBUFS, because a caller that sized `work` for the bracket it
 *             meant to support did nothing wrong; it doubles as the version-bump guard, since a
 *             Wuffs that changed this arithmetic would otherwise stop decoding for a reason
 *             nothing could report.
 *
 * The bytes are treated as a whole file rather than the start of a stream, so a truncated PNG is
 * -EILSEQ here instead of a request for more that nobody can answer.
 */
int inkwell_png_decode_bgra(struct inkwell_png_decoder *decoder, const uint8_t *encoded, size_t len,
                            uint32_t width, uint32_t height, uint8_t *pixels, size_t pixels_len,
                            uint8_t *work, size_t work_len);

#ifdef __cplusplus
}
#endif
