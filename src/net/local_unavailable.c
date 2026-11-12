/* net/local.h where the loop has no Unix-domain socket path yet (Windows): every call refuses. */

#include "inkwell/net/local.h"

#include <errno.h>
#include <string.h>

int inkwell_local_peer(int fd, struct inkwell_local_peer *out) {
    (void)fd;
    if (out != NULL) {
        out->pid = out->uid = out->gid = -1;
    }
    return -ENOSYS;
}

int inkwell_local_listen(struct inkwell_local_listener *listener, struct inkwell_loop *loop,
                         const char *path, inkwell_local_accept_fn on_accept, void *userdata) {
    (void)loop;
    (void)path;
    (void)on_accept;
    (void)userdata;
    if (listener != NULL) {
        memset(listener, 0, sizeof *listener);
        listener->fd = -1;
    }
    return -ENOSYS;
}

void inkwell_local_listener_close(struct inkwell_local_listener *listener) {
    (void)listener;
}

int inkwell_local_connect(const char *path) {
    (void)path;
    return -ENOSYS;
}

int inkwell_local_conn_open(struct inkwell_local_conn *conn, int fd, struct inkwell_loop *loop,
                            const struct inkwell_local_conn_storage *storage,
                            inkwell_local_frame_fn on_frame, inkwell_local_closed_fn on_closed,
                            void *userdata) {
    (void)fd;
    (void)loop;
    (void)storage;
    (void)on_frame;
    (void)on_closed;
    (void)userdata;
    if (conn != NULL) {
        memset(conn, 0, sizeof *conn);
    }
    return -ENOSYS;
}

int inkwell_local_conn_send(struct inkwell_local_conn *conn, const uint8_t *payload, size_t len) {
    (void)conn;
    (void)payload;
    (void)len;
    return -ENOSYS;
}

int inkwell_local_conn_send_fds(struct inkwell_local_conn *conn, const uint8_t *payload, size_t len,
                                const int *fds, size_t count) {
    (void)conn;
    (void)payload;
    (void)len;
    (void)fds;
    (void)count;
    return -ENOSYS;
}

int inkwell_local_conn_take_fd(struct inkwell_local_conn *conn) {
    (void)conn;
    return -ENOSYS;
}

void inkwell_local_conn_close(struct inkwell_local_conn *conn) {
    (void)conn;
}

bool inkwell_local_conn_is_open(const struct inkwell_local_conn *conn) {
    (void)conn;
    return false;
}
