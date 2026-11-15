#include "inkwell/io/dm.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* dm-verity's parameter line: plain text, built everywhere. The ioctls are dm_linux.c. */

/* `len` bytes as lowercase hex at `out`, which has room for 2 * len + 1. */
static void hex(const uint8_t *bytes, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0U; i < len; ++i) {
        out[2U * i] = digits[bytes[i] >> 4];
        out[2U * i + 1U] = digits[bytes[i] & 0x0fU];
    }
    out[2U * len] = '\0';
}

#define DIGEST_MAX 64U /* sha512's; the kernel takes any digest its crypto API has */

int inkwell_dm_verity_params(const struct inkwell_dm_verity *v, char *out, size_t size) {
    if (v == NULL || out == NULL || v->data_device == NULL || v->hash_device == NULL ||
        v->algorithm == NULL || v->root_digest == NULL || v->root_digest_len == 0U ||
        v->root_digest_len > DIGEST_MAX || v->salt_len > DIGEST_MAX ||
        (v->salt_len > 0U && v->salt == NULL) || v->data_block_size == 0U ||
        v->hash_block_size == 0U || v->data_blocks == 0U) {
        return -EINVAL;
    }
    char root[2U * DIGEST_MAX + 1U];
    char salt[2U * DIGEST_MAX + 1U];
    hex(v->root_digest, v->root_digest_len, root);
    /* No salt is written "-", as veritysetup does. */
    if (v->salt_len > 0U) {
        hex(v->salt, v->salt_len, salt);
    } else {
        snprintf(salt, sizeof salt, "-");
    }
    const int n = snprintf(out, size, "1 %s %s %u %u %llu %llu %s %s %s%s%s", v->data_device,
                           v->hash_device, (unsigned)v->data_block_size,
                           (unsigned)v->hash_block_size, (unsigned long long)v->data_blocks,
                           (unsigned long long)v->hash_start_block, v->algorithm, root, salt,
                           v->options != NULL ? " " : "", v->options != NULL ? v->options : "");
    if (n < 0) {
        return -EINVAL;
    }
    if ((size_t)n >= size) {
        return -ENOSPC;
    }
    return n;
}
