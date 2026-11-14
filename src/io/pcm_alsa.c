#include "inkwell/io/pcm.h"

#include "inkwell/base/ioctl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sound/asound.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * The ALSA kernel interface, spoken directly. The kernel's hw_params is a set of constraints -
 * a bit mask for each choice (access, format), an interval for each number (rate, period) - that
 * the caller narrows and the kernel narrows further until every interval is one value. We narrow
 * everything to what we want except the period size, which is a minimum the kernel rounds up to
 * what its DMA can do. The same steps tinyalsa takes.
 */

static struct snd_mask *mask_of(struct snd_pcm_hw_params *p, int param) {
    return &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *interval_of(struct snd_pcm_hw_params *p, int param) {
    return &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* Everything allowed: every mask full, every interval [0, max]. */
static void params_any(struct snd_pcm_hw_params *p) {
    memset(p, 0, sizeof *p);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++n) {
        struct snd_mask *m = mask_of(p, n);
        memset(m->bits, 0xff, sizeof m->bits);
    }
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++n) {
        struct snd_interval *i = interval_of(p, n);
        i->min = 0;
        i->max = UINT_MAX;
    }
    p->rmask = UINT_MAX;
    p->cmask = 0;
    p->info = UINT_MAX;
}

static void params_mask(struct snd_pcm_hw_params *p, int param, unsigned bit) {
    struct snd_mask *m = mask_of(p, param);
    memset(m->bits, 0, sizeof m->bits);
    m->bits[bit >> 5] |= 1U << (bit & 31U);
}

static void params_exact(struct snd_pcm_hw_params *p, int param, unsigned value) {
    struct snd_interval *i = interval_of(p, param);
    i->min = value;
    i->max = value;
    i->integer = 1;
}

static void params_min(struct snd_pcm_hw_params *p, int param, unsigned value) {
    interval_of(p, param)->min = value;
}

/* The one value an interval was narrowed to, or 0 when it was not narrowed to one. */
static unsigned params_get(struct snd_pcm_hw_params *p, int param) {
    const struct snd_interval *i = interval_of(p, param);
    return i->integer && i->min == i->max ? i->min : 0;
}

static int read_file(const char *path, char *buf, size_t size) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    size_t used = 0;
    while (used + 1 < size) {
        const ssize_t n = read(fd, buf + used, size - 1 - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
    }
    buf[used] = '\0';
    close(fd);
    return 0;
}

int inkwell_pcm_find_playback(struct inkwell_pcm_device *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    char text[4096];
    if (read_file("/proc/asound/pcm", text, sizeof text) != 0) {
        return -ENOENT; /* no sound support in the kernel at all, which is no devices */
    }
    return inkwell_pcm_parse_list(text, out, 1) == 1 ? 0 : -ENOENT;
}

static int configure(int fd, const struct inkwell_pcm_config *want, struct inkwell_pcm *pcm) {
    struct snd_pcm_hw_params hw;
    params_any(&hw);
    params_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    params_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    params_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    params_exact(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    params_exact(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16U * want->channels);
    params_exact(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, want->channels);
    params_exact(&hw, SNDRV_PCM_HW_PARAM_RATE, want->rate);
    params_exact(&hw, SNDRV_PCM_HW_PARAM_PERIODS, want->periods);
    params_min(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, want->period_frames);
    if (ioctl(fd, inkwell_ioctl_request_of(SNDRV_PCM_IOCTL_HW_PARAMS), &hw) != 0) {
        return -errno;
    }
    const unsigned period = params_get(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    const unsigned periods = params_get(&hw, SNDRV_PCM_HW_PARAM_PERIODS);
    if (period == 0 || periods < 2) {
        return -EINVAL; /* the kernel left a choice open, which it should not */
    }
    pcm->config = *want;
    pcm->config.period_frames = period;
    pcm->config.periods = periods;
    pcm->buffer_frames = period * periods;

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof sw);
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = period;
    /* Start when the buffer is full, so a stream does not start on one period and run dry. */
    sw.start_threshold = pcm->buffer_frames;
    sw.stop_threshold = pcm->buffer_frames;
    sw.silence_threshold = 0;
    sw.silence_size = 0;
    snd_pcm_uframes_t boundary = pcm->buffer_frames;
    while (boundary * 2 <= (snd_pcm_uframes_t)INT_MAX - pcm->buffer_frames) {
        boundary *= 2;
    }
    sw.boundary = boundary;
    if (ioctl(fd, inkwell_ioctl_request_of(SNDRV_PCM_IOCTL_SW_PARAMS), &sw) != 0) {
        return -errno;
    }
    if (ioctl(fd, inkwell_ioctl_request_of(SNDRV_PCM_IOCTL_PREPARE)) != 0) {
        return -errno;
    }
    return 0;
}

int inkwell_pcm_open(struct inkwell_pcm *pcm, unsigned card, unsigned device,
                     const struct inkwell_pcm_config *want) {
    if (pcm == NULL) {
        return -EINVAL;
    }
    memset(pcm, 0, sizeof *pcm);
    pcm->fd = -1;
    if (want == NULL || want->rate == 0 || want->channels == 0 || want->period_frames == 0 ||
        want->periods < 2) {
        return -EINVAL;
    }
    char path[48];
    (void)snprintf(path, sizeof path, "/dev/snd/pcmC%uD%up", card, device);
    const int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    const int configured = configure(fd, want, pcm);
    if (configured != 0) {
        close(fd);
        memset(pcm, 0, sizeof *pcm);
        pcm->fd = -1;
        return configured;
    }
    pcm->fd = fd;
    return 0;
}

/* After the stream stopped - an underrun (-EPIPE) or a system suspend (-ESTRPIPE) - makes it
   ready to take frames again. */
static int recover(struct inkwell_pcm *pcm, int error) {
    if (error == -ESTRPIPE) {
        /* Resumed in place where the device can; otherwise prepared afresh below. A device that is
           still waking says EAGAIN, and the next write tries again rather than this one waiting. */
        if (ioctl(pcm->fd, inkwell_ioctl_request_of(SNDRV_PCM_IOCTL_RESUME)) == 0) {
            return 0;
        }
        if (errno == EAGAIN) {
            return 0;
        }
    } else if (error == -EPIPE) {
        ++pcm->underruns;
    } else {
        return error;
    }
    return ioctl(pcm->fd, inkwell_ioctl_request_of(SNDRV_PCM_IOCTL_PREPARE)) == 0 ? 0 : -errno;
}

int64_t inkwell_pcm_write(struct inkwell_pcm *pcm, const int16_t *frames, size_t count) {
    if (pcm == NULL || pcm->fd < 0 || (frames == NULL && count > 0)) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        struct snd_xferi xfer = {.result = 0, .buf = (void *)frames, .frames = count};
        if (ioctl(pcm->fd, inkwell_ioctl_request_of(SNDRV_PCM_IOCTL_WRITEI_FRAMES), &xfer) == 0) {
            return (int64_t)xfer.result;
        }
        const int error = -errno;
        if (error == -EAGAIN) {
            return 0;
        }
        if (error == -EINTR) {
            continue;
        }
        const int recovered = recover(pcm, error);
        if (recovered != 0) {
            return recovered;
        }
    }
    return 0;
}

void inkwell_pcm_close(struct inkwell_pcm *pcm) {
    if (pcm == NULL || pcm->fd < 0) {
        return;
    }
    (void)ioctl(pcm->fd, inkwell_ioctl_request_of(SNDRV_PCM_IOCTL_DROP));
    close(pcm->fd);
    pcm->fd = -1;
}
