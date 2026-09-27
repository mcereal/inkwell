#include "inkwell/base/wipe.h"

#include <string.h>

/* Called through a volatile pointer, which the compiler must read at the call and so cannot
   prove is memset: the store is no longer one it may elide. Mbed TLS's
   mbedtls_platform_zeroize() takes the same way, for the same portability - C17 has no
   memset_s() a compiler is bound to, and explicit_bzero() is not everywhere. */
static void *(*const volatile k_memset)(void *, int, size_t) = memset;

void inkwell_wipe(void *bytes, size_t len) {
    if (bytes != NULL && len > 0U) {
        (void)k_memset(bytes, 0, len);
    }
}
