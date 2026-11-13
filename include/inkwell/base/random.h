#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bytes from the operating system's secure random source: getrandom() on Linux, arc4random_buf()
 * on macOS, rand_s() - the system RNG behind the CRT - on Windows. For a salt, a key, a token:
 * anything an attacker must not be able to guess. Never a seeded PRNG, which would be the same
 * sequence on every boot of a device with no clock.
 *
 * Fills all `len` bytes or none: 0, or a negative errno (and `out` untouched past what the call
 * says, which is to say not to be used). A Linux kernel whose pool is not yet seeded makes the
 * call wait for it, which is the right answer for a secret.
 */
int inkwell_random_bytes(void *out, size_t len);

#ifdef __cplusplus
}
#endif
