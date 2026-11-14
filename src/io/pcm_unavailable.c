#include "inkwell/io/pcm.h"

#include <errno.h>
#include <string.h>

/* PCM playback here is ALSA's kernel interface, which only Linux has. macOS plays through Core
 * Audio and Windows through WASAPI, each a callback on a thread of its own and its own
 * vocabulary; either would be a backend of its own, so until one is wanted every open refuses.
 * The list parser in pcm.c is plain text and works everywhere. */
int inkwell_pcm_find_playback(struct inkwell_pcm_device *out) {
    return out == NULL ? -EINVAL : -ENOSYS;
}

int inkwell_pcm_open(struct inkwell_pcm *pcm, unsigned card, unsigned device,
                     const struct inkwell_pcm_config *want) {
    (void)card;
    (void)device;
    (void)want;
    if (pcm == NULL) {
        return -EINVAL;
    }
    memset(pcm, 0, sizeof *pcm);
    pcm->fd = -1;
    return -ENOSYS;
}

int64_t inkwell_pcm_write(struct inkwell_pcm *pcm, const int16_t *frames, size_t count) {
    (void)pcm;
    (void)frames;
    (void)count;
    return -EINVAL; /* never open */
}

void inkwell_pcm_close(struct inkwell_pcm *pcm) {
    (void)pcm;
}
