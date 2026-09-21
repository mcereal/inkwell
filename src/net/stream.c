#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "inkwell/net/stream.h"

#include "inkwell/base/log.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define INKWELL_STREAM_READ_CHUNK 1024U
/* Reads per loop turn. A peer that sends a burst - a database sync, a backlog after a
   reconnection - would otherwise hold the loop for as long as it kept talking, and everything
   else on it, including whatever is reading input from a person, waits. */
#define INKWELL_STREAM_READS_PER_TURN 8U

int inkwell_stream_init(struct inkwell_stream *stream, const char *tag,
                        struct inkwell_stream_slot *slots, size_t slot_count, uint8_t *bytes,
                        size_t slot_bytes) {
    if (stream == NULL) {
        return -EINVAL;
    }
    memset(stream, 0, sizeof *stream);
    stream->fd = -1;
    stream->tag = tag != NULL ? tag : "stream";
    /* Either half missing means no queue at all, rather than a queue with a hole in it: a
       caller that only listens passes nothing, and one that passes half of it has a bug worth
       failing on the first send rather than on the first partial write. */
    if (slots != NULL && bytes != NULL && slot_count > 0U && slot_bytes > 0U) {
        stream->slots = slots;
        stream->bytes = bytes;
        stream->slot_count = slot_count;
        stream->slot_bytes = slot_bytes;
    }
    return 0;
}

void inkwell_stream_set_sink(struct inkwell_stream *stream, inkwell_stream_bytes_fn on_bytes,
                             inkwell_stream_dropped_fn on_dropped, void *userdata) {
    if (stream == NULL) {
        return;
    }
    stream->on_bytes = on_bytes;
    stream->on_dropped = on_dropped;
    stream->userdata = userdata;
}

bool inkwell_stream_is_open(const struct inkwell_stream *stream) {
    return stream != NULL && stream->fd >= 0;
}

size_t inkwell_stream_bytes_received(const struct inkwell_stream *stream) {
    return stream != NULL ? stream->bytes_received : 0U;
}

size_t inkwell_stream_queued(const struct inkwell_stream *stream) {
    return stream != NULL ? stream->queued : 0U;
}

/*
 * The one write in this file, so the SIGPIPE suppression cannot be forgotten on one path and
 * applied on another. MSG_NOSIGNAL is a socket flag and a tty write takes none, which is the
 * whole of why the stream is told what it holds.
 */
static ssize_t stream_write(const struct inkwell_stream *stream, const uint8_t *data, size_t len) {
    if (stream->kind == INKWELL_STREAM_SOCKET) {
        return send(stream->fd, data, len, MSG_NOSIGNAL);
    }
    return write(stream->fd, data, len);
}

/* ------------------------------------------------------------------ the queue */

static uint8_t *slot_bytes_at(const struct inkwell_stream *stream, size_t index) {
    return stream->bytes + (index * stream->slot_bytes);
}

static void stream_drop_queue(struct inkwell_stream *stream) {
    for (size_t i = 0; i < stream->queued; ++i) {
        const size_t index = (stream->head + i) % stream->slot_count;
        const uint32_t id = stream->slots[index].id;
        if (id != 0U && stream->on_dropped != NULL) {
            stream->on_dropped(stream->userdata, id);
        }
    }
    stream->head = 0U;
    stream->queued = 0U;
}

/* Keeps EPOLLOUT armed exactly while the queue has a remainder, so a descriptor that filled up
   wakes the loop instead of waiting out the poll timeout. */
static void stream_update_write_interest(struct inkwell_stream *stream) {
    if (!stream->fd_registered || stream->loop == NULL) {
        return;
    }
    const bool want = stream->queued > 0U;
    if (want == stream->want_write) {
        return;
    }
    const uint32_t events = want ? (uint32_t)(EPOLLIN | EPOLLOUT) : (uint32_t)EPOLLIN;
    if (inkwell_loop_update_fd(stream->loop, stream->fd, events) == 0) {
        stream->want_write = want;
    }
}

int inkwell_stream_flush(struct inkwell_stream *stream) {
    if (stream == NULL) {
        return -EINVAL;
    }
    if (stream->fd < 0) {
        return -ENOTCONN;
    }

    while (stream->queued > 0U) {
        struct inkwell_stream_slot *slot = &stream->slots[stream->head];
        const ssize_t written = stream_write(
            stream, slot_bytes_at(stream, stream->head) + slot->sent, slot->length - slot->sent);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* the far end is not draining; EPOLLOUT brings us back */
            }
            if (errno == EINTR) {
                continue;
            }
            /* EPIPE lands here rather than as a dead process, which is what MSG_NOSIGNAL above
               bought: the far end went away and the owner gets to say so in its own words. */
            inkwell_log_warn(stream->tag, "write failed: %s", strerror(errno));
            return -EIO;
        }

        slot->sent += (size_t)written;
        if (slot->sent < slot->length) {
            break;
        }
        stream->head = (stream->head + 1U) % stream->slot_count;
        stream->queued -= 1U;
    }

    stream_update_write_interest(stream);
    return 0;
}

int inkwell_stream_send(struct inkwell_stream *stream, const uint8_t *data, size_t len,
                        uint32_t id) {
    if (stream == NULL || data == NULL) {
        return -EINVAL;
    }
    if (stream->fd < 0) {
        return -ENOTCONN;
    }
    if (stream->slot_count == 0U || stream->queued >= stream->slot_count) {
        return -ENOSPC;
    }
    if (len > stream->slot_bytes) {
        return -EMSGSIZE;
    }

    const size_t index = (stream->head + stream->queued) % stream->slot_count;
    memcpy(slot_bytes_at(stream, index), data, len);
    stream->slots[index].length = len;
    stream->slots[index].sent = 0U;
    stream->slots[index].id = id;
    stream->queued += 1U;

    stream_update_write_interest(stream);
    return inkwell_stream_flush(stream);
}

int inkwell_stream_write_raw(struct inkwell_stream *stream, const uint8_t *data, size_t len) {
    if (stream == NULL || data == NULL) {
        return -EINVAL;
    }
    if (stream->fd < 0) {
        return -ENOTCONN;
    }
    const ssize_t written = stream_write(stream, data, len);
    if (written < 0) {
        return -errno;
    }
    return (int)written;
}

/* ------------------------------------------------------------------ read path */

int inkwell_stream_pump(struct inkwell_stream *stream) {
    if (stream == NULL) {
        return -EINVAL;
    }
    if (stream->fd < 0) {
        return -ENOTCONN;
    }

    size_t total = 0U;
    for (unsigned turn = 0U; turn < INKWELL_STREAM_READS_PER_TURN; ++turn) {
        uint8_t buffer[INKWELL_STREAM_READ_CHUNK];
        const ssize_t got = read(stream->fd, buffer, sizeof buffer);
        if (got > 0) {
            total += (size_t)got;
            stream->bytes_received += (size_t)got;
            if (stream->on_bytes != NULL) {
                stream->on_bytes(stream->userdata, buffer, (size_t)got);
            }
            continue;
        }
        if (got == 0) {
            /* On a socket or a pipe this is unambiguously EOF. On a tty it is EOF only because
               the caller was required to configure VMIN = 1 - see the note by
               enum inkwell_stream_kind, which is where the reason lives. */
            return -ENOTCONN;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        inkwell_log_warn(stream->tag, "read failed: %s", strerror(errno));
        return -EIO;
    }

    return (int)total;
}

/* ------------------------------------------------------------------ lifecycle */

int inkwell_stream_open(struct inkwell_stream *stream, int fd, enum inkwell_stream_kind kind,
                        struct inkwell_loop *loop, inkwell_loop_callback callback, void *userdata) {
    if (stream == NULL || fd < 0) {
        return -EINVAL;
    }
    if (stream->fd >= 0) {
        return -EBUSY;
    }

    if (loop != NULL) {
        const int added = inkwell_loop_add_fd(loop, fd, EPOLLIN, callback, userdata);
        if (added < 0) {
            return added;
        }
        stream->fd_registered = true;
    } else {
        stream->fd_registered = false;
    }

    stream->fd = fd;
    stream->kind = kind;
    stream->loop = loop;
    stream->want_write = false;
    stream->bytes_received = 0U;
    stream->head = 0U;
    stream->queued = 0U;
    return 0;
}

void inkwell_stream_close(struct inkwell_stream *stream) {
    if (stream == NULL) {
        return;
    }
    /* The queue is reported even on an already-closed stream: a caller that queued and then
       failed to open still owes those writes a verdict. */
    stream_drop_queue(stream);
    if (stream->fd < 0) {
        return;
    }
    if (stream->fd_registered && stream->loop != NULL) {
        inkwell_loop_remove_fd(stream->loop, stream->fd);
    }
    close(stream->fd);
    stream->fd = -1;
    stream->fd_registered = false;
    stream->want_write = false;
}
