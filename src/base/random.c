#include "inkwell/base/random.h"

#include "platform.h"

#include <errno.h>

int inkwell_random_bytes(void *out, size_t len) {
    if (len == 0U) {
        return 0;
    }
    if (out == NULL) {
        return -EINVAL;
    }
    return inkwell_platform_random(out, len);
}
