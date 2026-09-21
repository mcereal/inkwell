#pragma once

/*
 * A byte stream on a descriptor, driven by the loop.
 *
 * Every transport that carries a protocol over a stream does the same four things: read what is
 * ready without starving the rest of the loop, hand the bytes up, queue what goes out, and keep
 * INKWELL_LOOP_OUT armed exactly while that queue has a remainder. Underneath that there is no
 * protocol at all - a tty and a socket differ in how a write is made and in nothing else.
 *
 * This is that half, and only that half. **It knows nothing about framing.** Bytes arrive as
 * bytes and go out as bytes; whatever turns them into messages belongs to whoever owns the
 * stream, because a length prefix, a sentinel and a line ending are three answers to a question
 * this layer does not ask.
 *
 * It is a component rather than a policy. It decides nothing about when to connect or what to
 * do when a link dies, and it never resets itself: pump() and flush() report a fatal error and
 * stop. The owner decides what that means, because "the port was unplugged" and "the peer hung
 * up" are the same errno and two different sentences.
 */

#include "inkwell/runtime/loop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the descriptor is, which decides how a write to it is made.
 *
 * Not a detail: writing to a socket whose peer has gone raises `SIGPIPE`, whose default
 * disposition kills the process - so a peer that drops off the network between two turns of the
 * loop would take the program down with it, before the `-EPIPE` this code handles could ever be
 * returned. `send(MSG_NOSIGNAL)` is the suppression, and it is a socket call: on a tty it fails
 * with `ENOTSOCK`, which is why the stream has to be told which kind it holds rather than
 * guessing. A tty needs none of it - a write to an unplugged port is `EIO`, not a signal.
 */
enum inkwell_stream_kind {
    INKWELL_STREAM_FILE = 0, /* a tty or a pipe: write() */
    INKWELL_STREAM_SOCKET,   /* a socket: send() with MSG_NOSIGNAL */
};

/*
 * **A tty handed to this must be configured so that an empty read blocks or gives EAGAIN, not
 * zero.** In practice that means `VMIN = 1` in noncanonical mode, on an `O_NONBLOCK`
 * descriptor.
 *
 * This is a real requirement rather than a preference, and it is the caller's because the
 * caller is the only one that can meet it. `read()` returning 0 is the only way a descriptor
 * says "the far end is gone", and a noncanonical tty with `VMIN = 0` returns 0 the moment its
 * input queue is empty - which is every quiet moment on a working serial port. The two are
 * indistinguishable from in here, so a stream configured that way would report -ENOTCONN on a
 * device that is sitting there perfectly happy, and its owner would tear down a live link.
 *
 * With `VMIN = 1` an empty queue on a non-blocking descriptor is a proper `EAGAIN`, and 0 means
 * the port really did go away. A socket or a pipe needs none of this: 0 is unambiguously EOF on
 * both.
 */

/* One queued write, with a cursor for a partial one. The bytes live in the caller's buffer;
   see inkwell_stream_init(). */
struct inkwell_stream_slot {
    size_t length;
    size_t sent;
    /* The caller's name for this write, handed back if it is dropped unsent. 0 means "nothing
       to report", which is what a write nobody is waiting on passes. */
    uint32_t id;
};

/*
 * Bytes that arrived. Called from pump(), possibly several times in one call, never reentrantly.
 * `bytes` is only valid for the duration of the call.
 */
typedef void (*inkwell_stream_bytes_fn)(void *userdata, const uint8_t *bytes, size_t len);

/*
 * A queued write that will never go out, because the stream closed with it still queued.
 *
 * Called once per dropped slot that carried a non-zero id, in the order they were queued. A
 * stream that is torn down owes an answer to everything it accepted and did not send, and the
 * only thing this layer can say about one is that it did not happen.
 */
typedef void (*inkwell_stream_dropped_fn)(void *userdata, uint32_t id);

struct inkwell_stream {
    int fd;
    enum inkwell_stream_kind kind;
    bool fd_registered;
    bool want_write; /* INKWELL_LOOP_OUT is armed because the queue has a remainder */
    struct inkwell_loop *loop;

    /* The caller's queue storage. See inkwell_stream_init() for why it is not in here. */
    struct inkwell_stream_slot *slots;
    uint8_t *bytes;
    size_t slot_count;
    size_t slot_bytes;
    size_t head;
    size_t queued;

    size_t bytes_received;

    inkwell_stream_bytes_fn on_bytes;
    inkwell_stream_dropped_fn on_dropped;
    void *userdata;

    /* What this stream's log lines are filed under ("serial", "tcp"). Borrowed and never freed;
       callers pass a literal. */
    const char *tag;
};

/*
 * Puts the stream in its closed state and adopts the caller's queue storage.
 *
 * **The queue is the caller's memory, and that is the whole shape of this component.** How many
 * writes may be in flight and how large one may be are two numbers a platform layer has no way
 * to answer: they come from a protocol's largest message and from how much an application is
 * willing to hold while a descriptor is not draining. A stream that declared them would be one
 * program's answer compiled into everybody else's, and the first caller that needed a different
 * one would fork the file.
 *
 * So: `slots` is `slot_count` entries, `bytes` is `slot_count * slot_bytes`, both live as long
 * as the stream, and a write larger than `slot_bytes` is refused with -EMSGSIZE rather than
 * truncated. Passing NULL for either, or 0 for either count, gives a stream that can read but
 * refuses every send with -ENOSPC, which is a legitimate configuration for a caller that only
 * listens.
 *
 * `tag` is borrowed; pass a literal. Returns 0, or -EINVAL.
 */
int inkwell_stream_init(struct inkwell_stream *stream, const char *tag,
                        struct inkwell_stream_slot *slots, size_t slot_count, uint8_t *bytes,
                        size_t slot_bytes);

/*
 * Where arriving bytes go, and who hears about a write that was dropped. Either may be NULL -
 * a NULL `on_bytes` reads and counts and discards, which is what a caller draining a descriptor
 * it no longer cares about wants. Survives an open/close cycle, so it is set here rather than
 * at open.
 */
void inkwell_stream_set_sink(struct inkwell_stream *stream, inkwell_stream_bytes_fn on_bytes,
                             inkwell_stream_dropped_fn on_dropped, void *userdata);

/*
 * Adopts `fd` - which must already be open and non-blocking - and watches it for readability.
 * The stream owns the descriptor from here: close() is inkwell_stream_close()'s to call.
 *
 * `kind` says what the descriptor is, which decides how writes are made; see above, and get it
 * wrong towards SOCKET and every write fails with ENOTSOCK.
 *
 * `callback` and `userdata` go to the event loop unchanged, so the owner keeps its own dispatch.
 * A NULL `loop` opens the stream unwatched, which is what a test driving pump() by hand wants.
 * Returns 0, -EINVAL, -EBUSY when one is already open, or a negative errno from the loop with
 * the descriptor left alone for the caller to close.
 */
int inkwell_stream_open(struct inkwell_stream *stream, int fd, enum inkwell_stream_kind kind,
                        struct inkwell_loop *loop, inkwell_loop_callback callback, void *userdata);

/* Unwatches and closes the descriptor, and reports every queued write as dropped. Safe on a
   stream that is already closed - which still reports the queue, because a caller that queued
   and then failed to open is owed those answers too. */
void inkwell_stream_close(struct inkwell_stream *stream);
bool inkwell_stream_is_open(const struct inkwell_stream *stream);

/*
 * Reads what is ready and hands it to the sink, bounded so one busy descriptor cannot starve
 * the rest of the loop. Returns the byte count, 0 when nothing was ready, or:
 *
 *   -ENOTCONN  the far end is gone (EOF), or the stream is not open
 *   -EIO       the read failed
 *
 * Both are fatal and neither closes the stream: the owner decides.
 */
int inkwell_stream_pump(struct inkwell_stream *stream);

/* Writes as much of the queue as the descriptor will take. Returns 0, -ENOTCONN when the stream
   is closed, or -EIO on a failed write, which is fatal in the same way. */
int inkwell_stream_flush(struct inkwell_stream *stream);

/*
 * Queues `len` bytes - exactly as they should appear on the wire - and flushes.
 *
 * `id` is the caller's name for this write, handed back through the dropped callback if the
 * stream closes with it still queued; pass 0 for a write nobody is waiting on. Returns 0,
 * -ENOSPC when the queue is full, -EMSGSIZE when it will not fit a slot, -ENOTCONN, or -EIO.
 */
int inkwell_stream_send(struct inkwell_stream *stream, const uint8_t *data, size_t len,
                        uint32_t id);

/*
 * Writes straight at the descriptor, outside the queue and ahead of anything in it.
 *
 * For the byte runs that are deliberately not messages - a break, a wake burst, a sequence
 * whose whole purpose is that a parser cannot complete anything out of it. Best effort, and a
 * short write is not retried. Returns the bytes written or a negative errno.
 */
int inkwell_stream_write_raw(struct inkwell_stream *stream, const uint8_t *data, size_t len);

/* Bytes read since the current descriptor was opened. */
size_t inkwell_stream_bytes_received(const struct inkwell_stream *stream);
/* How many writes are queued, which is what a caller checks before deciding it is backed up. */
size_t inkwell_stream_queued(const struct inkwell_stream *stream);

#ifdef __cplusplus
}
#endif
