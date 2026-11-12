#pragma once

/*
 * Messages between processes on one machine: a Unix-domain socket at a path, framed.
 *
 * Two programs on the same machine that want to exchange messages need less than two on a
 * network - no name to resolve, no session to secure - and one thing more: to know *who* is on
 * the other end. A socket at a path gives both. The kernel says which process connected, and
 * which user it runs as, so a server can decide what a client may ask without the client having
 * to claim anything.
 *
 * Three pieces, each usable alone:
 *
 *   - **The listener** binds a path and hands every accepted connection to its owner as a
 *     descriptor, with the peer's identity read at accept - before the peer can have exited and
 *     had its pid reused.
 *   - **The connection** is a stream (net/stream.h) with the length-prefixed frames of
 *     codec/frame.h on it: the owner sends a payload and is called with each whole one that
 *     arrives. It does not know what a payload means.
 *   - **inkwell_local_peer()** is the identity on its own, for a descriptor obtained otherwise.
 *
 * **A stale path is cleared, a live one is not.** A server that crashed leaves its socket file
 * behind, and bind() on it fails. The listener removes the file only when it is a socket and a
 * connect to it is refused - nobody is listening - so a second copy of a running server fails
 * with -EADDRINUSE rather than silently stealing the first one's clients, and a regular file at
 * the path is never deleted.
 *
 * The check is a real connect, which has three edges. A live server sees it as a connection that
 * closes at once. On macOS a live listener whose backlog is full also refuses, so a server too
 * busy to accept can look stale. And two servers starting together can both find the file stale
 * and clear it in turn, leaving the first bound to a file nobody can reach. A deployment that
 * can start a second copy of a server while the first is busy or starting puts a lock beside the
 * path.
 *
 * **The memory is the caller's**, as it is for the stream: the queue and the frame buffer are
 * sized by the protocol on top, which this layer cannot know.
 *
 * Unix-domain sockets are POSIX here. Windows has them too, but not on the loop's socket path
 * yet, and every call refuses with -ENOSYS there.
 */

#include "inkwell/codec/frame.h"
#include "inkwell/net/stream.h"
#include "inkwell/runtime/loop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sun_path is 104 bytes on macOS and 108 on Linux; a path has to fit the smaller. */
#define INKWELL_LOCAL_PATH_MAX 104U

/* Who is on the other end, as the kernel says. -1 where this system cannot tell. */
struct inkwell_local_peer {
    int64_t pid;
    int64_t uid;
    int64_t gid;
};

/*
 * The peer of a connected Unix-domain socket: SO_PEERCRED on Linux; getpeereid() and
 * LOCAL_PEERPID on macOS. Returns 0 with whatever the system knows (a field it does not is -1),
 * or a negative errno. The pid is the one at connect() time - a pid can be reused once its
 * process exits, which is why the listener reads it at accept rather than later.
 */
int inkwell_local_peer(int fd, struct inkwell_local_peer *out);

/*
 * A connection accepted. `fd` is the caller's from here: non-blocking, close-on-exec, ready for
 * inkwell_local_conn_open() or to be closed straight away by a server that does not want it.
 *
 * The callback may close the listener, but must not free it until the callback returns: the
 * accept loop looks at it once more.
 */
typedef void (*inkwell_local_accept_fn)(void *userdata, int fd,
                                        const struct inkwell_local_peer *peer);

struct inkwell_local_listener {
    int fd;
    struct inkwell_loop *loop;
    char path[INKWELL_LOCAL_PATH_MAX];
    /* The socket file this listener bound, so close removes that file and not one somebody else
       has since put at the path. */
    uint64_t path_dev;
    uint64_t path_ino;
    inkwell_local_accept_fn on_accept;
    void *userdata;
};

/*
 * Binds `path` and listens on it, accepting on the loop. The path is left with the process's
 * umask; a server that wants other users to reach it chmods it after this returns. `listener` is
 * overwritten, so it must not be listening already - close it first.
 *
 * Returns 0; -ENAMETOOLONG; -EADDRINUSE when a server is already listening there; -EEXIST when
 * something that is not a socket is at the path; or the negative errno of the call that failed.
 */
int inkwell_local_listen(struct inkwell_local_listener *listener, struct inkwell_loop *loop,
                         const char *path, inkwell_local_accept_fn on_accept, void *userdata);

/* Stops listening and removes the path - only while the file there is still the one this listener
   bound. Connections already accepted are not affected. Safe on a listener that is not
   listening. */
void inkwell_local_listener_close(struct inkwell_local_listener *listener);

/*
 * Connects to the listener at `path`. A Unix-domain connect completes at once, so the result is
 * a connected, non-blocking descriptor or a negative errno: -ENOENT when nothing is at the path,
 * -ECONNREFUSED when nobody is listening. A full backlog is -EAGAIN on Linux; macOS refuses
 * instead, with -ECONNREFUSED.
 */
int inkwell_local_connect(const char *path);

/* The memory a connection runs in. `slots` and `bytes` are the stream's queue (net/stream.h); a
   message takes one slot, its 4-byte header and its payload together, so the largest payload
   sent is `slot_bytes` - INKWELL_FRAME_HEADER and `slot_count` is the messages that may wait.
   `frame` holds one arriving payload, the largest one accepted being `frame_capacity`.

   `fds` holds descriptors that arrived and have not been taken yet, `fd_capacity` of them; NULL
   and 0 is a connection that takes none, on which a descriptor a peer sends is closed on
   arrival. All of it lives as long as the connection. */
struct inkwell_local_conn_storage {
    struct inkwell_stream_slot *slots;
    size_t slot_count;
    uint8_t *bytes;
    size_t slot_bytes;
    uint8_t *frame;
    size_t frame_capacity;
    int *fds;
    size_t fd_capacity;
};

/* One whole payload, valid for the call. To stop hearing from the peer, close the connection
   here: nothing after it is delivered. */
typedef void (*inkwell_local_frame_fn)(void *userdata, const uint8_t *payload, size_t len);

/* The connection ended without the owner closing it: the peer hung up (-ENOTCONN), a read or
   write failed (-EIO), or the peer sent a frame larger than the capacity (-EMSGSIZE). The
   connection is already closed when this is called, and the owner may free it here. A peer
   that sends more descriptors than the connection has room for, or whose descriptors the
   kernel could not deliver, is ended with -EMSGSIZE too: past that point no message can be
   matched with its descriptors. */
typedef void (*inkwell_local_closed_fn)(void *userdata, int reason);

struct inkwell_local_conn {
    struct inkwell_stream stream;
    struct inkwell_frame_reader reader;
    struct inkwell_local_peer peer;
    int *fds; /* arrived and not taken: a ring of fd_capacity, fd_count from fd_head */
    size_t fd_capacity;
    size_t fd_head;
    size_t fd_count;
    inkwell_local_frame_fn on_frame;
    inkwell_local_closed_fn on_closed;
    void *userdata;
    int failure;        /* why the connection is ending, while the loop callback unwinds */
    bool dispatching;   /* inside the loop callback, where a close has to wait for the pump */
    bool close_pending; /* closed from a callback; done when the pump returns */
};

/*
 * Adopts `fd` - connected and non-blocking, from the listener or inkwell_local_connect() - and
 * watches it on `loop`. Each whole payload goes to `on_frame`, in order; an end the owner did not
 * ask for goes to `on_closed`. The peer is read here and kept in `conn->peer`.
 *
 * `conn` is overwritten, so it must not be open already - close it first.
 *
 * The owner may close the connection from inside either callback - nothing more is delivered
 * after that - but must not free it until the callback returns, except in on_closed.
 *
 * Returns 0 or a negative errno, in which case `fd` is still the caller's to close.
 */
int inkwell_local_conn_open(struct inkwell_local_conn *conn, int fd, struct inkwell_loop *loop,
                            const struct inkwell_local_conn_storage *storage,
                            inkwell_local_frame_fn on_frame, inkwell_local_closed_fn on_closed,
                            void *userdata);

/*
 * Queues one payload as one frame, header and payload in one slot: -ENOSPC when the queue is
 * full, -EMSGSIZE when the payload and its header do not fit a slot, -ENOTCONN when
 * the connection is closed, -EIO on a write that failed. A failed write is not closed here - the
 * caller may be inside a callback - and the end it means reaches on_closed when the loop next
 * hears the socket.
 */
int inkwell_local_conn_send(struct inkwell_local_conn *conn, const uint8_t *payload, size_t len);

/*
 * Descriptors with a message - the way one process hands another memory to map or a file it
 * could not open (base/fd.h).
 *
 * send_fds() is send() with up to INKWELL_FD_PASS_MAX descriptors attached to the message; the
 * caller keeps its own, since the connection sends duplicates. Besides send()'s errors, -EBADF
 * for one that is not open.
 *
 * The receiving end gets them in arrival order, ahead of the message that carried them, and
 * takes them one at a time with take_fd() - from on_frame, for the message the protocol says
 * carries them. **How many a message carries is the protocol's to say**, as Wayland's does:
 * this layer only keeps them in order. take_fd() returns a descriptor the caller now owns, or
 * -ENOENT when none is waiting. Any not taken are closed with the connection.
 */
int inkwell_local_conn_send_fds(struct inkwell_local_conn *conn, const uint8_t *payload, size_t len,
                                const int *fds, size_t count);
int inkwell_local_conn_take_fd(struct inkwell_local_conn *conn);

/* Closes the connection without calling on_closed: the owner asked. Safe when already closed. */
void inkwell_local_conn_close(struct inkwell_local_conn *conn);

bool inkwell_local_conn_is_open(const struct inkwell_local_conn *conn);

#ifdef __cplusplus
}
#endif
