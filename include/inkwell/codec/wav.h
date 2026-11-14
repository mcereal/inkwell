#pragma once

/*
 * A WAV file's sound, as 16-bit stereo frames.
 *
 * A WAV is RIFF chunks: `fmt ` says how the samples are laid out, `data` holds them, and a `LIST`
 * of `INFO` may name the piece. inkwell_wav_open() walks the chunks through a read callback - a
 * descriptor's pread, a buffer in memory - so it never needs the whole file, and a tag written
 * after the samples is still found. What it reports is where the samples are and how to read
 * them; inkwell_wav_s16() turns a run of them into what a mixer takes.
 *
 * Integer PCM of 8, 16, 24 and 32 bits and 32-bit float, plain or WAVE_FORMAT_EXTENSIBLE, at any
 * rate and one to eight channels. Mono is played on both sides, and more than two channels are
 * read as their first two (front left and right, in the order WAV keeps them). Anything else -
 * ADPCM, mu-law, a RIFF that is not a WAVE - is refused rather than played as noise.
 *
 * Pure: bytes in, frames out, no allocation and no descriptor of its own. The file is whatever
 * the caller was given, and every length in it is checked against the file's own size, since a
 * WAV is a file somebody else wrote.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INKWELL_WAV_TAG_MAX 64U
#define INKWELL_WAV_CHANNELS_MAX 8U

/* Reads up to `len` bytes at `at`: how many were read (0 at the end), or a negative errno. */
typedef int64_t (*inkwell_wav_read_fn)(void *ctx, uint64_t at, void *buf, size_t len);

enum inkwell_wav_encoding {
    INKWELL_WAV_PCM = 1,   /* signed integers; unsigned at 8 bits */
    INKWELL_WAV_FLOAT = 3, /* IEEE 754, 32-bit */
};

struct inkwell_wav {
    enum inkwell_wav_encoding encoding;
    uint32_t rate;        /* frames a second */
    uint32_t channels;    /* 1..INKWELL_WAV_CHANNELS_MAX */
    uint32_t bits;        /* per sample: 8, 16, 24 or 32 */
    uint32_t frame_bytes; /* one sample of every channel */
    uint64_t data_at;     /* where the first frame starts */
    uint64_t frames;      /* how many there are, whole ones only */
    /* The LIST INFO tags, NUL-terminated, "" when the file has none: INAM, IART and IPRD. */
    char title[INKWELL_WAV_TAG_MAX];
    char artist[INKWELL_WAV_TAG_MAX];
    char album[INKWELL_WAV_TAG_MAX];
};

/*
 * Reads a WAV's layout. `size` is the file's length, which bounds every chunk. 0; -EINVAL for
 * something that is not a WAV or is cut short before its samples; -ENOTSUP for a WAV in an
 * encoding this does not play; or the read's own negative errno.
 */
int inkwell_wav_open(struct inkwell_wav *wav, uint64_t size, inkwell_wav_read_fn read, void *ctx);

/* The length in milliseconds, rounded down. */
uint64_t inkwell_wav_duration_ms(const struct inkwell_wav *wav);

/*
 * `frames` frames of `src`, laid out as `wav` says, into `dst` as interleaved 16-bit stereo -
 * `2 * frames` samples. Float is clipped to full scale; deeper integers keep their top 16 bits.
 */
void inkwell_wav_s16(const struct inkwell_wav *wav, const void *src, size_t frames, int16_t *dst);

#ifdef __cplusplus
}
#endif
