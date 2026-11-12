#include "inkwell/net/local.h"

#include "inkwell/base/fd.h"
#include "inkwell/base/log.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Accepts per wake, so a burst of connects cannot hold the loop. The rest are still queued in
   the kernel and wake it again. */
#define LOCAL_ACCEPTS_PER_TURN 16U

static int local_address(const char *path, struct sockaddr_un *address, socklen_t *len) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }
    const size_t n = strlen(path);
    if (n >= INKWELL_LOCAL_PATH_MAX || n >= sizeof address->sun_path) {
        return -ENAMETOOLONG;
    }
    memset(address, 0, sizeof *address);
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, n + 1U);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1U);
    return 0;
}

int inkwell_local_peer(int fd, struct inkwell_local_peer *out) {
    if (fd < 0 || out == NULL) {
        return -EINVAL;
    }
    struct inkwell_fd_peer peer;
    const int read = inkwell_fd_peer(fd, &peer);
    out->pid = peer.pid;
    out->uid = peer.uid;
    out->gid = peer.gid;
    return read;
}

int inkwell_local_connect(const char *path) {
    struct sockaddr_un address;
    socklen_t len;
    const int named = local_address(path, &address, &len);
    if (named < 0) {
        return named;
    }
    const int fd = inkwell_fd_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return fd;
    }
    if (connect(fd, (const struct sockaddr *)&address, len) != 0) {
        const int error = errno == EWOULDBLOCK ? EAGAIN : errno;
        (void)inkwell_fd_close(fd);
        return -error;
    }
    return fd;
}

/* ------------------------------------------------------------------ the listener */

static int on_listener_ready(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    struct inkwell_local_listener *listener = userdata;
    for (unsigned turn = 0U; turn < LOCAL_ACCEPTS_PER_TURN && listener->fd >= 0; ++turn) {
        const int client = accept(listener->fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                inkwell_log_warn("local", "accept on %s: %s", listener->path, strerror(errno));
            }
            return 0;
        }
        if (inkwell_fd_set_nonblocking_cloexec(client) != 0) {
            (void)inkwell_fd_close(client);
            continue;
        }
        struct inkwell_local_peer peer;
        (void)inkwell_local_peer(client, &peer);
        if (listener->on_accept != NULL) {
            listener->on_accept(listener->userdata, client, &peer);
        } else {
            (void)inkwell_fd_close(client);
        }
    }
    return 0;
}

/* Whether the socket file at `path` is somebody's live listener. A connect that is refused means
   the file outlived its server. */
static bool path_is_live(const char *path) {
    const int fd = inkwell_local_connect(path);
    if (fd >= 0) {
        (void)inkwell_fd_close(fd);
        return true;
    }
    return fd != -ECONNREFUSED && fd != -ENOENT;
}

int inkwell_local_listen(struct inkwell_local_listener *listener, struct inkwell_loop *loop,
                         const char *path, inkwell_local_accept_fn on_accept, void *userdata) {
    if (listener == NULL || loop == NULL) {
        return -EINVAL;
    }
    memset(listener, 0, sizeof *listener);
    listener->fd = -1;
    struct sockaddr_un address;
    socklen_t len;
    const int named = local_address(path, &address, &len);
    if (named < 0) {
        return named;
    }
    const int fd = inkwell_fd_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return fd;
    }
    if (bind(fd, (const struct sockaddr *)&address, len) != 0) {
        int error = errno;
        struct stat st;
        if (error == EADDRINUSE && lstat(path, &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                error = EEXIST; /* not ours to delete */
            } else if (!path_is_live(path)) {
                /* A server that died left this behind; nobody answers on it. */
                if (unlink(path) != 0) {
                    error = errno;
                } else if (bind(fd, (const struct sockaddr *)&address, len) != 0) {
                    error = errno;
                } else {
                    error = 0;
                }
            }
        }
        if (error != 0) {
            (void)inkwell_fd_close(fd);
            return -error;
        }
    }
    if (listen(fd, SOMAXCONN) != 0) {
        const int error = errno;
        (void)unlink(path);
        (void)inkwell_fd_close(fd);
        return -error;
    }
    struct stat bound;
    if (lstat(path, &bound) != 0) {
        const int error = errno;
        (void)unlink(path);
        (void)inkwell_fd_close(fd);
        return -error;
    }
    const int added = inkwell_loop_add_fd(loop, fd, INKWELL_LOOP_IN, on_listener_ready, listener);
    if (added < 0) {
        (void)unlink(path);
        (void)inkwell_fd_close(fd);
        return added;
    }
    listener->fd = fd;
    listener->path_dev = (uint64_t)bound.st_dev;
    listener->path_ino = (uint64_t)bound.st_ino;
    listener->loop = loop;
    memcpy(listener->path, address.sun_path, sizeof listener->path);
    listener->path[sizeof listener->path - 1U] = '\0';
    listener->on_accept = on_accept;
    listener->userdata = userdata;
    return 0;
}

void inkwell_local_listener_close(struct inkwell_local_listener *listener) {
    if (listener == NULL || listener->fd < 0) {
        return;
    }
    (void)inkwell_loop_remove_fd(listener->loop, listener->fd);
    (void)inkwell_fd_close(listener->fd);
    /* Another server may have the path by now - one that found this one unanswering and cleared
       it - and its file is not ours to remove. */
    struct stat st;
    if (lstat(listener->path, &st) == 0 && S_ISSOCK(st.st_mode) &&
        (uint64_t)st.st_dev == listener->path_dev && (uint64_t)st.st_ino == listener->path_ino) {
        (void)unlink(listener->path);
    }
    listener->fd = -1;
}

/* ------------------------------------------------------------------ the connection */

static void conn_finish_close(struct inkwell_local_conn *conn) {
    inkwell_stream_close(&conn->stream);
    conn->close_pending = false;
    conn->failure = 0;
}

static void conn_bytes(void *userdata, const uint8_t *bytes, size_t len);

static int conn_ready(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct inkwell_local_conn *conn = userdata;
    conn->dispatching = true;
    if ((events & INKWELL_LOOP_OUT) != 0U && inkwell_stream_flush(&conn->stream) < 0) {
        conn->failure = -EIO;
    }
    if (conn->failure == 0 && !conn->close_pending &&
        (events & (INKWELL_LOOP_IN | INKWELL_LOOP_HUP | INKWELL_LOOP_ERR)) != 0U) {
        /* Everything the peer sent before it hung up is delivered before the hang-up is. */
        const int pumped = inkwell_stream_pump(&conn->stream);
        if (conn->failure == 0 && pumped < 0) {
            conn->failure = pumped == -ENOTCONN ? -ENOTCONN : -EIO;
        }
    }
    conn->dispatching = false;
    if (conn->close_pending) {
        conn_finish_close(conn); /* the owner asked: no on_closed */
        return 0;
    }
    if (conn->failure != 0) {
        const int reason = conn->failure;
        conn_finish_close(conn);
        if (conn->on_closed != NULL) {
            conn->on_closed(conn->userdata, reason); /* last: the owner may free conn here */
        }
    }
    return 0;
}

static int conn_frame(void *userdata, const uint8_t *payload, size_t len) {
    struct inkwell_local_conn *conn = userdata;
    if (conn->close_pending) {
        return 1;
    }
    if (conn->on_frame != NULL) {
        conn->on_frame(conn->userdata, payload, len);
    }
    return conn->close_pending ? 1 : 0;
}

static void conn_bytes(void *userdata, const uint8_t *bytes, size_t len) {
    struct inkwell_local_conn *conn = userdata;
    if (conn->close_pending || conn->failure != 0) {
        return; /* the pump keeps reading after a close; what it reads is nobody's */
    }
    const int fed = inkwell_frame_reader_feed(&conn->reader, bytes, len, conn_frame, conn);
    if (fed == -EMSGSIZE) {
        conn->failure = -EMSGSIZE;
    }
}

int inkwell_local_conn_open(struct inkwell_local_conn *conn, int fd, struct inkwell_loop *loop,
                            const struct inkwell_local_conn_storage *storage,
                            inkwell_local_frame_fn on_frame, inkwell_local_closed_fn on_closed,
                            void *userdata) {
    if (conn == NULL || fd < 0 || loop == NULL || storage == NULL || storage->frame == NULL ||
        storage->frame_capacity == 0U) {
        return -EINVAL;
    }
    memset(conn, 0, sizeof *conn);
    const int ready = inkwell_stream_init(&conn->stream, "local", storage->slots,
                                          storage->slot_count, storage->bytes, storage->slot_bytes);
    if (ready < 0) {
        return ready;
    }
    inkwell_frame_reader_init(&conn->reader, storage->frame, storage->frame_capacity);
    (void)inkwell_local_peer(fd, &conn->peer);
    conn->on_frame = on_frame;
    conn->on_closed = on_closed;
    conn->userdata = userdata;
    inkwell_stream_set_sink(&conn->stream, conn_bytes, NULL, conn);
    return inkwell_stream_open(&conn->stream, fd, INKWELL_STREAM_SOCKET, loop, conn_ready, conn);
}

bool inkwell_local_conn_is_open(const struct inkwell_local_conn *conn) {
    return conn != NULL && inkwell_stream_is_open(&conn->stream) && !conn->close_pending;
}

int inkwell_local_conn_send(struct inkwell_local_conn *conn, const uint8_t *payload, size_t len) {
    if (conn == NULL || (payload == NULL && len > 0U)) {
        return -EINVAL;
    }
    if (!inkwell_local_conn_is_open(conn)) {
        return -ENOTCONN;
    }
    uint8_t header[INKWELL_FRAME_HEADER];
    const int framed = inkwell_frame_header(header, len);
    if (framed < 0) {
        return framed;
    }
    /* Header and payload in one slot: one write, and never half a frame queued. */
    return inkwell_stream_send_prefixed(&conn->stream, header, sizeof header, payload, len, 0U);
}

void inkwell_local_conn_close(struct inkwell_local_conn *conn) {
    if (conn == NULL) {
        return;
    }
    if (conn->dispatching) {
        conn->close_pending = true;
        return;
    }
    conn_finish_close(conn);
}
