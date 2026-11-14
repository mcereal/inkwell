/*
 * Reading a WAV: the layout from its chunks, its tags wherever they are, and its samples as 16-bit
 * stereo, against files built here byte by byte - so each case says exactly what it was handed.
 */

#include "framework/inkwell_test.h"

#include "inkwell/codec/wav.h"

#include <errno.h>
#include <string.h>

struct wav_file {
    uint8_t bytes[1024];
    size_t len;
    size_t riff_len_at;
};

static void put(struct wav_file *f, const void *p, size_t n) {
    memcpy(f->bytes + f->len, p, n);
    f->len += n;
}

static void put16(struct wav_file *f, uint32_t v) {
    const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(f, b, 2);
}

static void put32(struct wav_file *f, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(f, b, 4);
}

static void wav_begin(struct wav_file *f) {
    memset(f, 0, sizeof *f);
    put(f, "RIFF", 4);
    f->riff_len_at = f->len;
    put32(f, 0);
    put(f, "WAVE", 4);
}

static void wav_fmt(struct wav_file *f, uint32_t format, uint32_t channels, uint32_t rate,
                    uint32_t bits) {
    put(f, "fmt ", 4);
    put32(f, 16);
    put16(f, format);
    put16(f, channels);
    put32(f, rate);
    put32(f, rate * channels * bits / 8U);
    put16(f, channels * bits / 8U);
    put16(f, bits);
}

static void wav_data(struct wav_file *f, const void *samples, uint32_t n) {
    put(f, "data", 4);
    put32(f, n);
    put(f, samples, n);
    if ((n & 1U) != 0U) {
        put(f, "", 1);
    }
}

static void wav_info(struct wav_file *f, const char *title, const char *artist) {
    const uint32_t tn = (uint32_t)strlen(title) + 1U;
    const uint32_t an = (uint32_t)strlen(artist) + 1U;
    put(f, "LIST", 4);
    put32(f, 4U + 8U + tn + (tn & 1U) + 8U + an + (an & 1U));
    put(f, "INFO", 4);
    put(f, "INAM", 4);
    put32(f, tn);
    put(f, title, tn);
    if ((tn & 1U) != 0U) {
        put(f, "", 1);
    }
    put(f, "IART", 4);
    put32(f, an);
    put(f, artist, an);
    if ((an & 1U) != 0U) {
        put(f, "", 1);
    }
}

static void wav_end(struct wav_file *f) {
    const uint32_t n = (uint32_t)f->len - 8U;
    memcpy(f->bytes + f->riff_len_at,
           (uint8_t[4]){(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)}, 4);
}

static int64_t read_mem(void *ctx, uint64_t at, void *buf, size_t len) {
    const struct wav_file *f = ctx;
    if (at >= f->len) {
        return 0;
    }
    const size_t n = len < f->len - at ? len : f->len - (size_t)at;
    memcpy(buf, f->bytes + at, n);
    return (int64_t)n;
}

static int open_mem(struct inkwell_wav *wav, struct wav_file *f) {
    return inkwell_wav_open(wav, f->len, read_mem, f);
}

INKWELL_TEST_CASE(wav_reads_16_bit_stereo, unit) {
    struct wav_file f;
    wav_begin(&f);
    wav_fmt(&f, 1, 2, 44100, 16);
    const int16_t samples[] = {100, -100, 32767, -32768};
    wav_data(&f, samples, sizeof samples);
    wav_end(&f);

    struct inkwell_wav wav;
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != 0, "a plain WAV opens");
    INKWELL_TEST_FAIL_IF(wav.rate != 44100U || wav.channels != 2U || wav.bits != 16U,
                         "with its rate, channels and depth");
    INKWELL_TEST_FAIL_IF(wav.frames != 2U || wav.frame_bytes != 4U, "two frames of four bytes");
    INKWELL_TEST_FAIL_IF(wav.data_at != 44U, "the samples after the 44-byte header");
    INKWELL_TEST_FAIL_IF(wav.title[0] != '\0', "no tags: an empty title");

    int16_t out[4];
    inkwell_wav_s16(&wav, f.bytes + wav.data_at, 2, out);
    INKWELL_TEST_FAIL_IF(memcmp(out, samples, sizeof out) != 0, "16-bit stereo is copied as is");
    record_success(test_name);
}

INKWELL_TEST_CASE(wav_plays_mono_on_both_sides, unit) {
    struct wav_file f;
    wav_begin(&f);
    wav_fmt(&f, 1, 1, 8000, 8);
    const uint8_t samples[] = {128, 255, 0};
    wav_data(&f, samples, sizeof samples);
    wav_end(&f);

    struct inkwell_wav wav;
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != 0, "8-bit mono opens");
    INKWELL_TEST_FAIL_IF(wav.frames != 3U, "three frames, the pad byte not one of them");
    int16_t out[6];
    inkwell_wav_s16(&wav, f.bytes + wav.data_at, 3, out);
    INKWELL_TEST_FAIL_IF(out[0] != 0 || out[1] != 0, "128 is silence, unsigned at 8 bits");
    INKWELL_TEST_FAIL_IF(out[2] != 32512 || out[3] != 32512, "the top, on both sides");
    INKWELL_TEST_FAIL_IF(out[4] != -32768 || out[5] != -32768, "the bottom, on both sides");
    record_success(test_name);
}

INKWELL_TEST_CASE(wav_keeps_the_top_of_deeper_samples, unit) {
    struct wav_file f;
    wav_begin(&f);
    wav_fmt(&f, 1, 2, 48000, 24);
    const uint8_t samples[] = {0x11, 0x34, 0x12, 0x00, 0x00, 0x80};
    wav_data(&f, samples, sizeof samples);
    wav_end(&f);

    struct inkwell_wav wav;
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != 0, "24-bit opens");
    int16_t out[2];
    inkwell_wav_s16(&wav, f.bytes + wav.data_at, 1, out);
    INKWELL_TEST_FAIL_IF(out[0] != 0x1234 || out[1] != -32768, "the top 16 bits of each sample");

    wav_begin(&f);
    wav_fmt(&f, 3, 1, 48000, 32);
    const float floats[] = {0.5f, 2.0f, -3.0f};
    wav_data(&f, floats, sizeof floats);
    wav_end(&f);
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != 0, "float opens");
    INKWELL_TEST_FAIL_IF(wav.encoding != INKWELL_WAV_FLOAT, "as float");
    int16_t mono[6];
    inkwell_wav_s16(&wav, f.bytes + wav.data_at, 3, mono);
    INKWELL_TEST_FAIL_IF(mono[0] != 16384, "half scale");
    INKWELL_TEST_FAIL_IF(mono[2] != 32767 || mono[4] != -32767, "clipped, not wrapped");
    record_success(test_name);
}

INKWELL_TEST_CASE(wav_finds_tags_after_the_samples, unit) {
    struct wav_file f;
    wav_begin(&f);
    wav_fmt(&f, 1, 2, 48000, 16);
    const int16_t samples[] = {1, 2, 3, 4, 5, 6};
    wav_data(&f, samples, sizeof samples);
    wav_info(&f, "Morning", "The Inks");
    wav_end(&f);

    struct inkwell_wav wav;
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != 0, "a WAV with a LIST opens");
    INKWELL_TEST_FAIL_IF(strcmp(wav.title, "Morning") != 0, "its title, written after the data");
    INKWELL_TEST_FAIL_IF(strcmp(wav.artist, "The Inks") != 0, "and its artist");
    INKWELL_TEST_FAIL_IF(wav.frames != 3U, "the samples are still the data chunk's");
    INKWELL_TEST_FAIL_IF(inkwell_wav_duration_ms(&wav) != 0U, "three frames are under a ms");
    record_success(test_name);
}

INKWELL_TEST_CASE(wav_plays_what_a_cut_file_holds, unit) {
    struct wav_file f;
    wav_begin(&f);
    wav_fmt(&f, 1, 1, 1000, 16);
    put(&f, "data", 4);
    put32(&f, 0xFFFFFFFFU); /* a length never filled in, as a stream writes it */
    const int16_t samples[] = {1, 2, 3, 4, 5};
    put(&f, samples, sizeof samples);
    put(&f, "", 1); /* half a sample */
    wav_end(&f);

    struct inkwell_wav wav;
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != 0, "a data chunk longer than the file opens");
    INKWELL_TEST_FAIL_IF(wav.frames != 5U, "with the whole frames the file has");
    INKWELL_TEST_FAIL_IF(inkwell_wav_duration_ms(&wav) != 5U, "five ms at 1 kHz");
    record_success(test_name);
}

INKWELL_TEST_CASE(wav_refuses_what_it_cannot_play, unit) {
    struct wav_file f;
    struct inkwell_wav wav;

    wav_begin(&f);
    wav_fmt(&f, 2, 2, 44100, 4); /* Microsoft ADPCM */
    wav_data(&f, "\0\0\0\0", 4);
    wav_end(&f);
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != -ENOTSUP, "ADPCM is not played as noise");

    wav_begin(&f);
    wav_fmt(&f, 1, 2, 44100, 12);
    wav_data(&f, "\0\0\0", 3);
    wav_end(&f);
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != -ENOTSUP, "nor a depth it does not know");

    wav_begin(&f);
    wav_fmt(&f, 1, 0, 44100, 16);
    wav_data(&f, "\0\0", 2);
    wav_end(&f);
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != -EINVAL, "no channels is no WAV");

    wav_begin(&f);
    wav_fmt(&f, 1, 2, 44100, 16);
    wav_end(&f);
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != -EINVAL, "a format with no samples is cut short");

    wav_begin(&f);
    memcpy(f.bytes + 8, "AVI ", 4);
    wav_fmt(&f, 1, 2, 44100, 16);
    wav_data(&f, "\0\0\0\0", 4);
    wav_end(&f);
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != -EINVAL, "a RIFF that is not a WAVE is not one");

    INKWELL_TEST_FAIL_IF(inkwell_wav_open(&wav, 4U, read_mem, &f) != -EINVAL,
                         "four bytes are not a WAV");
    record_success(test_name);
}

INKWELL_TEST_CASE(wav_reads_the_extensible_format, unit) {
    struct wav_file f;
    wav_begin(&f);
    put(&f, "fmt ", 4);
    put32(&f, 40);
    put16(&f, 0xFFFE);
    put16(&f, 6); /* 5.1 */
    put32(&f, 48000);
    put32(&f, 48000U * 12U);
    put16(&f, 12);
    put16(&f, 16);
    put16(&f, 22);   /* cbSize */
    put16(&f, 16);   /* valid bits */
    put32(&f, 0x3F); /* channel mask */
    put16(&f, 1);    /* KSDATAFORMAT_SUBTYPE_PCM's first two bytes */
    put(&f, "\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
    const int16_t samples[] = {10, 20, 30, 40, 50, 60};
    wav_data(&f, samples, sizeof samples);
    wav_end(&f);

    struct inkwell_wav wav;
    INKWELL_TEST_FAIL_IF(open_mem(&wav, &f) != 0, "WAVE_FORMAT_EXTENSIBLE opens");
    INKWELL_TEST_FAIL_IF(wav.channels != 6U || wav.encoding != INKWELL_WAV_PCM, "as 6-channel PCM");
    int16_t out[2];
    inkwell_wav_s16(&wav, f.bytes + wav.data_at, 1, out);
    INKWELL_TEST_FAIL_IF(out[0] != 10 || out[1] != 20, "read as its front left and right");
    record_success(test_name);
}
