#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Zeroes `len` bytes at `bytes`, and is not optimised away. A plain memset() on a buffer nothing
 * reads again is a dead store a compiler may drop - under link-time optimisation even across a
 * caller's buffer - which is exactly the buffer that held a secret. NULL or 0 does nothing.
 */
void inkwell_wipe(void *bytes, size_t len);

#ifdef __cplusplus
}
#endif
