/*
 * A zlib stream of fixed-Huffman deflate (codec/deflate.h).
 *
 * RFC 1951 3.2.6 fixes the codes, so there is no tree to build: a literal or length is 7 to 9
 * bits by its range, a distance always 5, and every code is written most significant bit first
 * into a stream that is otherwise least significant bit first - hence the reversal.
 */

#include "inkwell/codec/deflate.h"

#include <string.h>

#define WINDOW 32768U
#define HASH_BITS 15U
#define HASH_SIZE (1U << HASH_BITS)
#define MIN_MATCH 3U
#define MAX_MATCH 258U
#define MAX_CHAIN 48U /* how far back along a chain a match is looked for */
#define NO_POS UINT32_MAX

struct bits {
    uint8_t *out;
    size_t cap, at;
    uint64_t acc;
    unsigned count;
    int full;
};

static void put(struct bits *b, uint32_t value, unsigned n) {
    b->acc |= (uint64_t)value << b->count;
    b->count += n;
    while (b->count >= 8U) {
        if (b->at >= b->cap) {
            b->full = 1;
            return;
        }
        b->out[b->at++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->count -= 8U;
    }
}

/* A Huffman code: `n` bits of `code`, most significant first. */
static void put_code(struct bits *b, uint32_t code, unsigned n) {
    uint32_t reversed = 0U;
    for (unsigned i = 0U; i < n; ++i) {
        reversed = (reversed << 1) | ((code >> i) & 1U);
    }
    put(b, reversed, n);
}

/* Literal or length symbol 0..287, by the fixed table. */
static void put_symbol(struct bits *b, unsigned sym) {
    if (sym <= 143U) {
        put_code(b, 0x30U + sym, 8U);
    } else if (sym <= 255U) {
        put_code(b, 0x190U + (sym - 144U), 9U);
    } else if (sym <= 279U) {
        put_code(b, sym - 256U, 7U);
    } else {
        put_code(b, 0xC0U + (sym - 280U), 8U);
    }
}

static const uint16_t k_len_base[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                        15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                        67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t k_dist_base[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(struct bits *b, unsigned len, unsigned dist) {
    unsigned i = 28U;
    while (k_len_base[i] > len) {
        --i;
    }
    put_symbol(b, 257U + i);
    put(b, len - k_len_base[i], k_len_extra[i]);
    unsigned d = 29U;
    while (k_dist_base[d] > dist) {
        --d;
    }
    put_code(b, d, 5U);
    put(b, dist - k_dist_base[d], k_dist_extra[d]);
}

static uint32_t hash3(const uint8_t *p) {
    const uint32_t v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    return (v * 2654435761U) >> (32U - HASH_BITS);
}

size_t inkwell_deflate_bound(size_t len) {
    if (len > (SIZE_MAX - 64U) / 9U * 8U) {
        return 0U;
    }
    return len + len / 8U + 64U;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

size_t inkwell_deflate_zlib(const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                            uint32_t *work) {
    if ((in == NULL && len > 0U) || out == NULL || work == NULL || cap < 6U) {
        return 0U;
    }
    uint32_t *head = work;
    uint32_t *prev = work + HASH_SIZE;
    for (size_t i = 0U; i < HASH_SIZE; ++i) {
        head[i] = NO_POS;
    }
    out[0] = 0x78U; /* deflate, a 32 KiB window */
    out[1] = 0x01U; /* the fastest level, and a check that makes the pair a multiple of 31 */
    struct bits b = {.out = out, .cap = cap - 4U, .at = 2U};
    put(&b, 1U, 1U); /* the final block */
    put(&b, 1U, 2U); /* fixed codes */

    size_t pos = 0U;
    while (pos < len && !b.full) {
        unsigned best_len = 0U;
        unsigned best_dist = 0U;
        if (pos + MIN_MATCH <= len) {
            const uint32_t h = hash3(&in[pos]);
            uint32_t cand = head[h];
            const size_t limit = len - pos < MAX_MATCH ? len - pos : MAX_MATCH;
            for (unsigned chain = 0U; cand != NO_POS && chain < MAX_CHAIN; ++chain) {
                const size_t dist = pos - cand;
                if (dist == 0U || dist > WINDOW) {
                    break;
                }
                if (in[cand + best_len] == in[pos + best_len]) {
                    size_t n = 0U;
                    while (n < limit && in[cand + n] == in[pos + n]) {
                        ++n;
                    }
                    if (n > best_len) {
                        best_len = (unsigned)n;
                        best_dist = (unsigned)dist;
                        if (n == limit) {
                            break;
                        }
                    }
                }
                const uint32_t next = prev[cand % WINDOW];
                if (next == NO_POS || next >= cand) {
                    break;
                }
                cand = next;
            }
            prev[pos % WINDOW] = head[h];
            head[h] = (uint32_t)pos;
        }
        if (best_len >= MIN_MATCH) {
            put_match(&b, best_len, best_dist);
            /* The positions a match covers go into the chains too, or a run is found once. */
            for (size_t i = pos + 1U; i < pos + best_len && i + MIN_MATCH <= len; ++i) {
                const uint32_t h = hash3(&in[i]);
                prev[i % WINDOW] = head[h];
                head[h] = (uint32_t)i;
            }
            pos += best_len;
        } else {
            put_symbol(&b, in[pos]);
            ++pos;
        }
    }
    put_symbol(&b, 256U); /* end of block */
    put(&b, 0U, 7U);      /* out to a byte */
    if (b.full) {
        return 0U;
    }
    /* Adler-32 of the input, big-endian, after the stream. */
    uint32_t s1 = 1U;
    uint32_t s2 = 0U;
    for (size_t i = 0U; i < len;) {
        const size_t run = len - i < 5552U ? len - i : 5552U;
        for (size_t j = 0U; j < run; ++j) {
            s1 += in[i + j];
            s2 += s1;
        }
        s1 %= 65521U;
        s2 %= 65521U;
        i += run;
    }
    put_be32(&out[b.at], s2 << 16 | s1);
    return b.at + 4U;
}
