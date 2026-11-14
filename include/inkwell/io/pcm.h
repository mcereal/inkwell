#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One PCM playback device: open it, agree a format with the kernel, and write interleaved
 * frames to it without blocking, on the loop's terms.
 *
 * On Linux this is the ALSA kernel interface itself - /dev/snd/pcmC<card>D<device>p and its
 * ioctls, the calls tinyalsa makes - and not alsa-lib, which is a dynamic library with a
 * configuration language of its own. Elsewhere every open refuses with -ENOSYS; the list parser
 * is plain text and works everywhere.
 *
 * The format is fixed at signed 16-bit little-endian, interleaved. A caller that mixes or
 * synthesises works in that format anyway, and one format is one less thing to agree.
 *
 * **The kernel may round.** A period of 256 frames may come back as 240 on a device whose DMA
 * wants a multiple of 48; the handle says what was agreed, and a caller sizes its buffers from
 * that, not from what it asked for. The rate and the channel count are not rounded: a device
 * that cannot do them exactly refuses, and the caller decides what to try next.
 *
 * **An underrun is recovered, not returned.** When the device runs dry (the caller was late with
 * a period), the kernel stops the stream; the next write prepares it again and carries on, and
 * the handle counts it. A caller that is late now and then should keep playing, not tear down.
 *
 * Playback starts once the device's buffer is full, so the first write should fill it - which
 * writing until the call returns 0 does.
 */

/* One playback device, as the kernel lists it. */
struct inkwell_pcm_device {
    unsigned card;
    unsigned device;
    char id[32];   /* the device's id, "virtio-snd", "ICH5" */
    char name[64]; /* its name */
};

/*
 * The playback devices in `text`, the contents of /proc/asound/pcm - a line each,
 * "00-00: id : name : playback 1 : capture 1" - into `out`, at most `max` of them. A line with no
 * "playback" stream is a capture-only device and is skipped. Returns how many, or -EINVAL for a
 * NULL argument. A malformed line is skipped, not fatal: the file is the kernel's, and a later
 * kernel may add a field.
 */
int inkwell_pcm_parse_list(const char *text, struct inkwell_pcm_device *out, size_t max);

/* The first playback device the kernel lists. 0, -ENOENT when there is none, or -ENOSYS where
   there is no ALSA. */
int inkwell_pcm_find_playback(struct inkwell_pcm_device *out);

struct inkwell_pcm_config {
    uint32_t rate;          /* frames per second, exactly: 48000 */
    uint32_t channels;      /* exactly: 2 */
    uint32_t period_frames; /* how much the device takes at a time; the kernel may round it */
    uint32_t periods;       /* how many periods the buffer holds, exactly: 2 or more */
};

struct inkwell_pcm {
    int fd;                           /* -1 while closed */
    struct inkwell_pcm_config config; /* what the kernel agreed */
    uint32_t buffer_frames;           /* period_frames * periods */
    uint64_t underruns;               /* recovered since the device was opened */
};

/*
 * Opens playback device `device` of card `card` in the format `want` says. 0, or a negative
 * errno with `pcm` closed: -EINVAL for a config that cannot be asked for (a zero, fewer than two
 * periods), -ENOENT for no such device, whatever the kernel says when it cannot do the format
 * (usually -EINVAL), -ENOSYS where there is no ALSA.
 */
int inkwell_pcm_open(struct inkwell_pcm *pcm, unsigned card, unsigned device,
                     const struct inkwell_pcm_config *want);

/* The descriptor to watch for room (INKWELL_LOOP_OUT): it is ready when a period fits. -1 while
   closed. */
int inkwell_pcm_fd(const struct inkwell_pcm *pcm);

/*
 * Writes up to `count` frames of `frames` (count * channels samples), as many as fit now.
 * Returns how many frames went, 0 when the buffer is full, or a negative errno for a device that
 * has gone (-ENODEV) or will not take the stream again.
 */
int64_t inkwell_pcm_write(struct inkwell_pcm *pcm, const int16_t *frames, size_t count);

/* Stops at once, dropping what is buffered, and closes. Safe on a closed handle. */
void inkwell_pcm_close(struct inkwell_pcm *pcm);

#ifdef __cplusplus
}
#endif
