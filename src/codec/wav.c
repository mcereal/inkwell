#include "inkwell/codec/wav.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

/* The most chunks walked before giving up on a file: a WAV has a handful, and a file that is
   nothing but empty chunks should not be walked for ever. */
#define WAV_CHUNKS_MAX 256U
#define WAV_FORMAT_EXTENSIBLE 0xFFFEU

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Exactly `len` bytes at `at`, or a negative errno: -EINVAL when the file ends first. */
static int read_exact(inkwell_wav_read_fn read, void *ctx, uint64_t at, void *buf, size_t len) {
    size_t got = 0U;
    while (got < len) {
        const int64_t n = read(ctx, at + got, (uint8_t *)buf + got, len - got);
        if (n < 0) {
            return (int)n;
        }
        if (n == 0) {
            return -EINVAL;
        }
        got += (size_t)n;
    }
    return 0;
}

static int parse_fmt(struct inkwell_wav *wav, const uint8_t *f, uint32_t len) {
    if (len < 16U) {
        return -EINVAL;
    }
    uint32_t format = le16(f);
    const uint32_t channels = le16(f + 2);
    const uint32_t rate = le32(f + 4);
    const uint32_t align = le16(f + 12);
    const uint32_t bits = le16(f + 14);
    if (format == WAV_FORMAT_EXTENSIBLE) {
        /* cbSize, valid bits, channel mask, then the sub-format GUID whose first two bytes are the
           format proper. */
        if (len < 40U) {
            return -EINVAL;
        }
        format = le16(f + 24);
    }
    if (channels == 0U || channels > INKWELL_WAV_CHANNELS_MAX || rate == 0U) {
        return -EINVAL;
    }
    if (format == INKWELL_WAV_PCM) {
        if (bits != 8U && bits != 16U && bits != 24U && bits != 32U) {
            return -ENOTSUP;
        }
    } else if (format == INKWELL_WAV_FLOAT) {
        if (bits != 32U) {
            return -ENOTSUP;
        }
    } else {
        return -ENOTSUP;
    }
    if (align != channels * (bits / 8U)) {
        return -EINVAL;
    }
    wav->encoding = (enum inkwell_wav_encoding)format;
    wav->channels = channels;
    wav->rate = rate;
    wav->bits = bits;
    wav->frame_bytes = align;
    return 0;
}

/* The INFO sub-chunks of a LIST: each an id, a length and a NUL-terminated string. */
static void parse_info(struct inkwell_wav *wav, const uint8_t *p, uint32_t len) {
    if (len < 4U || memcmp(p, "INFO", 4) != 0) {
        return;
    }
    uint32_t at = 4U;
    while (len - at >= 8U) {
        const uint8_t *id = p + at;
        const uint32_t n = le32(p + at + 4);
        at += 8U;
        if (n > len - at) {
            return;
        }
        char *to = NULL;
        if (memcmp(id, "INAM", 4) == 0) {
            to = wav->title;
        } else if (memcmp(id, "IART", 4) == 0) {
            to = wav->artist;
        } else if (memcmp(id, "IPRD", 4) == 0) {
            to = wav->album;
        }
        if (to != NULL) {
            size_t k = 0U;
            while (k < n && k + 1U < INKWELL_WAV_TAG_MAX && p[at + k] != '\0') {
                to[k] = (char)p[at + k];
                k++;
            }
            to[k] = '\0';
        }
        at += n + (n & 1U);
        if (at > len) {
            return;
        }
    }
}

int inkwell_wav_open(struct inkwell_wav *wav, uint64_t size, inkwell_wav_read_fn read, void *ctx) {
    memset(wav, 0, sizeof *wav);
    uint8_t head[12];
    if (size < sizeof head) {
        return -EINVAL;
    }
    int r = read_exact(read, ctx, 0U, head, sizeof head);
    if (r != 0) {
        return r;
    }
    if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) {
        return -EINVAL;
    }
    /* The RIFF's own length may be wrong (a stream written before its end was known); the file's
       size is the one bound that cannot lie. */
    bool have_fmt = false;
    bool have_data = false;
    uint64_t data_len = 0U;
    uint64_t at = sizeof head;
    for (unsigned chunks = 0U; chunks < WAV_CHUNKS_MAX && size - at >= 8U; chunks++) {
        uint8_t ch[8];
        r = read_exact(read, ctx, at, ch, sizeof ch);
        if (r != 0) {
            return r;
        }
        at += 8U;
        uint64_t len = le32(ch + 4);
        const bool is_data = memcmp(ch, "data", 4) == 0;
        if (len > size - at) {
            if (!is_data) {
                break; /* a chunk cut off at the end: what came before still stands */
            }
            len = size - at; /* samples cut short, or a length never filled in: play what is here */
        }
        if (memcmp(ch, "fmt ", 4) == 0 && !have_fmt) {
            uint8_t f[40];
            const uint32_t n = len < sizeof f ? (uint32_t)len : (uint32_t)sizeof f;
            r = read_exact(read, ctx, at, f, n);
            if (r == 0) {
                r = parse_fmt(wav, f, n);
            }
            if (r != 0) {
                return r;
            }
            have_fmt = true;
        } else if (memcmp(ch, "LIST", 4) == 0 && len <= 4096U) {
            uint8_t list[4096];
            r = read_exact(read, ctx, at, list, (size_t)len);
            if (r != 0) {
                return r;
            }
            parse_info(wav, list, (uint32_t)len);
        } else if (is_data && !have_data) {
            have_data = true;
            wav->data_at = at;
            data_len = len;
        }
        at += len + (len & 1U);
        if (at > size) {
            break;
        }
    }
    if (!have_fmt || !have_data) {
        return -EINVAL;
    }
    wav->frames = data_len / wav->frame_bytes;
    return 0;
}

uint64_t inkwell_wav_duration_ms(const struct inkwell_wav *wav) {
    return wav->rate == 0U ? 0U : wav->frames * 1000U / wav->rate;
}

/* One sample at `p`, as a 16-bit one. */
static int16_t sample(const struct inkwell_wav *wav, const uint8_t *p) {
    if (wav->encoding == INKWELL_WAV_FLOAT) {
        float f;
        memcpy(&f, p, sizeof f);
        if (!(f > -1.0f)) { /* NaN too */
            f = f != f ? 0.0f : -1.0f;
        } else if (f > 1.0f) {
            f = 1.0f;
        }
        const float s = f * 32767.0f;
        return (int16_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
    }
    switch (wav->bits) {
    case 8U:
        return (int16_t)(((int)p[0] - 128) * 256);
    case 16U:
        return (int16_t)le16(p);
    case 24U:
        return (int16_t)le16(p + 1);
    default:
        return (int16_t)le16(p + 2);
    }
}

void inkwell_wav_s16(const struct inkwell_wav *wav, const void *src, size_t frames, int16_t *dst) {
    const uint8_t *p = src;
    const uint32_t step = wav->bits / 8U;
    for (size_t i = 0U; i < frames; i++, p += wav->frame_bytes) {
        const int16_t left = sample(wav, p);
        dst[2U * i] = left;
        dst[2U * i + 1U] = wav->channels > 1U ? sample(wav, p + step) : left;
    }
}
