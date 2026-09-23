#include "inkwell/runtime/loop.h"

#include "inkwell/base/log.h"
#include "inkwell/base/time.h"
#if defined(_WIN32)
#include "windows_handle.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

/*
 * Two backends behind one table of sources.
 *
 * epoll is the one that ships, and on Linux nothing is translated: loop.h's INKWELL_LOOP_* are
 * epoll's own bits, asserted below, so a mask goes into epoll_ctl() and comes back out of
 * epoll_wait() untouched.
 *
 * kqueue is for a development host - macOS, where the same UI runs in a window - and it differs
 * from epoll in the three ways the code below is shaped around. Reading and writing are separate
 * filters, so a mask becomes zero, one or two registrations and changing it is a diff. One
 * descriptor that is both readable and writable comes back as two events, so a batch is merged
 * per source before anything is dispatched, and a callback is still called once with the union
 * as it is under epoll. And end-of-file is a flag on a filter rather than an event of its own, so
 * it is mapped onto what epoll would have said; see kqueue_mask().
 */
#if defined(_WIN32)
#elif defined(__linux__)
#include <sys/epoll.h>

_Static_assert(INKWELL_LOOP_IN == EPOLLIN, "INKWELL_LOOP_IN is EPOLLIN");
_Static_assert(INKWELL_LOOP_OUT == EPOLLOUT, "INKWELL_LOOP_OUT is EPOLLOUT");
_Static_assert(INKWELL_LOOP_ERR == EPOLLERR, "INKWELL_LOOP_ERR is EPOLLERR");
_Static_assert(INKWELL_LOOP_HUP == EPOLLHUP, "INKWELL_LOOP_HUP is EPOLLHUP");
#else
#include <fcntl.h>
#include <sys/event.h>
#endif

/* ---- the backend ----------------------------------------------------------------------------- */

#if defined(_WIN32)

#define WINDOWS_SOCKET_TOKEN_BASE 0x50000000

static long socket_events(uint32_t events) {
    long wanted = FD_CLOSE;
    if ((events & INKWELL_LOOP_IN) != 0U) {
        wanted |= FD_READ | FD_ACCEPT;
    }
    if ((events & INKWELL_LOOP_OUT) != 0U) {
        wanted |= FD_WRITE | FD_CONNECT;
    }
    return wanted;
}

static int backend_open(void) {
    return 0;
}

static int backend_add(struct inkwell_loop *loop, struct inkwell_loop_source *source) {
    (void)loop;
    if (source->is_socket) {
        return WSAEventSelect((SOCKET)source->socket, (WSAEVENT)source->socket_event,
                              socket_events(source->events)) == SOCKET_ERROR
                   ? -EIO
                   : 0;
    }
    return inkwell_windows_handle_get(source->fd) == NULL ? -EBADF : 0;
}

static int backend_update(struct inkwell_loop *loop, struct inkwell_loop_source *source,
                          uint32_t was) {
    (void)loop;
    if (source->is_socket) {
        if (WSAEventSelect((SOCKET)source->socket, (WSAEVENT)source->socket_event,
                           socket_events(source->events)) == SOCKET_ERROR) {
            (void)WSAEventSelect((SOCKET)source->socket, (WSAEVENT)source->socket_event,
                                 socket_events(was));
            return -EIO;
        }
    }
    return 0;
}

static int backend_remove(struct inkwell_loop *loop, struct inkwell_loop_source *source) {
    (void)loop;
    if (source->is_socket) {
        (void)WSAEventSelect((SOCKET)source->socket, NULL, 0);
        (void)WSACloseEvent((WSAEVENT)source->socket_event);
        source->socket_event = NULL;
        source->socket = 0U;
        source->is_socket = false;
    }
    return 0;
}

#elif defined(__linux__)

static int backend_open(void) {
    const int fd = epoll_create1(EPOLL_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

static int backend_ctl(int poll_fd, int op, struct inkwell_loop_source *source, uint32_t events) {
    struct epoll_event event;
    event.events = events;
    event.data.ptr = source;
    return epoll_ctl(poll_fd, op, source->fd, &event) < 0 ? -errno : 0;
}

static int backend_add(struct inkwell_loop *loop, struct inkwell_loop_source *source) {
    return backend_ctl(loop->poll_fd, EPOLL_CTL_ADD, source, source->events);
}

static int backend_update(struct inkwell_loop *loop, struct inkwell_loop_source *source,
                          uint32_t was) {
    (void)was;
    return backend_ctl(loop->poll_fd, EPOLL_CTL_MOD, source, source->events);
}

static int backend_remove(struct inkwell_loop *loop, struct inkwell_loop_source *source) {
    return epoll_ctl(loop->poll_fd, EPOLL_CTL_DEL, source->fd, NULL) < 0 ? -errno : 0;
}

#else

static int backend_open(void) {
    const int fd = kqueue();
    if (fd < 0) {
        return -errno;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/*
 * Registers or drops each filter whose wanted state differs between `was` and `now`, and on a
 * failure undoes whatever it had already done, so the kernel is left as `was` described it.
 *
 * One kevent() per change rather than one for both: a change list without room for errors stops
 * at the first that fails, so a batch whose first entry was a stale EV_DELETE would silently skip
 * the EV_ADD behind it. The price is that the two can half-succeed - an IN -> OUT update that
 * dropped the read filter and then could not add the write one would leave the source hearing
 * nothing while its caller was told only "failed", and a failed add would leave a registration
 * pointing at a slot the table has already released for reuse. Hence the rollback.
 */
static int kqueue_change(int poll_fd, int fd, int16_t filter, bool add, void *source) {
    struct kevent change;
    EV_SET(&change, (uintptr_t)fd, filter, add ? EV_ADD : EV_DELETE, 0, 0, source);
    return kevent(poll_fd, &change, 1, NULL, 0, NULL) < 0 ? -errno : 0;
}

static int kqueue_apply(int poll_fd, struct inkwell_loop_source *source, uint32_t was,
                        uint32_t now) {
    const struct {
        uint32_t bit;
        int16_t filter;
    } filters[2] = {{INKWELL_LOOP_IN, EVFILT_READ}, {INKWELL_LOOP_OUT, EVFILT_WRITE}};
    bool applied[2] = {false, false};
    for (size_t i = 0; i < 2U; ++i) {
        const bool before = (was & filters[i].bit) != 0U;
        const bool after = (now & filters[i].bit) != 0U;
        if (before == after) {
            continue;
        }
        const int result = kqueue_change(poll_fd, source->fd, filters[i].filter, after, source);
        if (result < 0) {
            for (size_t j = 0; j < i; ++j) {
                if (applied[j]) {
                    const bool restore = (was & filters[j].bit) != 0U;
                    (void)kqueue_change(poll_fd, source->fd, filters[j].filter, restore, source);
                }
            }
            return result;
        }
        applied[i] = true;
    }
    return 0;
}

static int backend_add(struct inkwell_loop *loop, struct inkwell_loop_source *source) {
    return kqueue_apply(loop->poll_fd, source, 0U, source->events);
}

static int backend_update(struct inkwell_loop *loop, struct inkwell_loop_source *source,
                          uint32_t was) {
    return kqueue_apply(loop->poll_fd, source, was, source->events);
}

static int backend_remove(struct inkwell_loop *loop, struct inkwell_loop_source *source) {
    /*
     * The failure is not reported. A kqueue drops a descriptor's filters by itself when the
     * descriptor is closed - epoll only does once every duplicate is - so a caller that closed
     * before removing gets ENOENT here for a registration that is already gone, and the source is
     * released all the same.
     */
    (void)kqueue_apply(loop->poll_fd, source, source->events, 0U);
    return 0;
}

/*
 * One kqueue event as the epoll bits it stands for.
 *
 * EV_EOF on the read filter is always readable, as it is under epoll, and is also a hangup only
 * once nothing is left to read: epoll reports a socket whose peer has sent FIN as readable and
 * nothing more, and a caller that checks for a hangup before it reads must not lose the last
 * bytes because this backend said it sooner. On the write filter it is a hangup, and an error
 * too when the socket carries one - a refused connect() is EPOLLOUT | EPOLLERR | EPOLLHUP under
 * epoll, and net/fetch.c reads exactly that.
 */
static uint32_t kqueue_mask(const struct kevent *event) {
    if ((event->flags & EV_ERROR) != 0U) {
        return INKWELL_LOOP_ERR;
    }
    const bool eof = (event->flags & EV_EOF) != 0U;
    /* The socket's error rides in fflags on either filter once EV_EOF is set - a reset
       connection is ECONNRESET here - and epoll reports it as EPOLLERR whichever direction was
       being watched. */
    const uint32_t error = eof && event->fflags != 0U ? INKWELL_LOOP_ERR : 0U;
    if (event->filter == EVFILT_READ) {
        return INKWELL_LOOP_IN | error | (eof && event->data == 0 ? INKWELL_LOOP_HUP : 0U);
    }
    if (event->filter == EVFILT_WRITE) {
        uint32_t mask = INKWELL_LOOP_OUT;
        if (eof) {
            mask |= INKWELL_LOOP_HUP | error;
        }
        return mask;
    }
    return 0U;
}

#endif

/* ---- the table ------------------------------------------------------------------------------- */

static int find_source_index(const struct inkwell_loop *loop, int fd) {
    for (int i = 0; i < INKWELL_LOOP_MAX_SOURCES; ++i) {
        if (loop->sources[i].active && loop->sources[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

static int wake_callback(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    struct inkwell_loop *loop = (struct inkwell_loop *)userdata;
    const int drained = inkwell_wake_drain(&loop->wake);
    if (drained < 0) {
        inkwell_log_warn("loop", "wake read failed: %s", strerror(-drained));
    }
    loop->running = false;
    loop->stop_requested = true;
    return 0;
}

int inkwell_loop_init(struct inkwell_loop *loop) {
    if (loop == NULL) {
        return -EINVAL;
    }

    memset(loop, 0, sizeof *loop);
    loop->poll_fd = -1;
    loop->wake.fd = -1;
    loop->wake.write_fd = -1;

    loop->poll_fd = backend_open();
    if (loop->poll_fd < 0) {
        const int error = loop->poll_fd;
        inkwell_log_error("loop", "creating the poll set failed: %s", strerror(-error));
        loop->poll_fd = -1;
        return error;
    }

    const int woken = inkwell_wake_open(&loop->wake);
    if (woken < 0) {
        inkwell_log_error("loop", "creating the wake failed: %s", strerror(-woken));
#if !defined(_WIN32)
        close(loop->poll_fd);
#endif
        loop->poll_fd = -1;
        return woken;
    }

    int result = inkwell_loop_add_fd(loop, loop->wake.fd, INKWELL_LOOP_IN, wake_callback, loop);
    if (result < 0) {
        inkwell_log_error("loop", "Failed to register wake FD: %d", result);
        inkwell_wake_close(&loop->wake);
#if !defined(_WIN32)
        close(loop->poll_fd);
#endif
        loop->poll_fd = -1;
        return result;
    }

    loop->running = false;
    loop->stop_requested = false;

    return 0;
}

void inkwell_loop_shutdown(struct inkwell_loop *loop) {
    if (loop == NULL) {
        return;
    }

    for (int i = 0; i < INKWELL_LOOP_MAX_SOURCES; ++i) {
        if (loop->sources[i].active) {
            inkwell_loop_remove_fd(loop, loop->sources[i].fd);
        }
    }

    inkwell_wake_close(&loop->wake);

    if (loop->poll_fd >= 0) {
#if !defined(_WIN32)
        close(loop->poll_fd);
#endif
        loop->poll_fd = -1;
    }

    loop->running = false;
    loop->stop_requested = false;
}

int inkwell_loop_add_fd(struct inkwell_loop *loop, int fd, uint32_t events,
                        inkwell_loop_callback callback, void *userdata) {
    if (loop == NULL || fd < 0 || callback == NULL) {
        return -EINVAL;
    }

    if (find_source_index(loop, fd) >= 0) {
        return -EEXIST;
    }

    int slot = -1;
    for (int i = 0; i < INKWELL_LOOP_MAX_SOURCES; ++i) {
        if (!loop->sources[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        return -ENOSPC;
    }

    struct inkwell_loop_source *source = &loop->sources[slot];
    source->fd = fd;
    source->events = events;
    source->callback = callback;
    source->userdata = userdata;
    source->active = true;

    const int added = backend_add(loop, source);
    if (added < 0) {
        source->active = false;
        inkwell_log_error("loop", "watching fd %d failed: %s", fd, strerror(-added));
        return added;
    }

    return 0;
}

#if defined(_WIN32)
int inkwell_loop_add_socket(struct inkwell_loop *loop, uintptr_t socket, uint32_t events,
                            inkwell_loop_callback callback, void *userdata) {
    if (loop == NULL || socket == (uintptr_t)INVALID_SOCKET || callback == NULL) {
        return -EINVAL;
    }
    int slot = -1;
    for (int i = 0; i < INKWELL_LOOP_MAX_SOURCES; ++i) {
        if (loop->sources[i].active && loop->sources[i].is_socket &&
            loop->sources[i].socket == socket) {
            return -EEXIST;
        }
        if (!loop->sources[i].active && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return -ENOSPC;
    }
    const WSAEVENT event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT) {
        return -EIO;
    }
    struct inkwell_loop_source *source = &loop->sources[slot];
    source->fd = WINDOWS_SOCKET_TOKEN_BASE + slot;
    source->events = events;
    source->callback = callback;
    source->userdata = userdata;
    source->socket = socket;
    source->socket_event = (void *)event;
    source->is_socket = true;
    source->active = true;
    const int added = backend_add(loop, source);
    if (added < 0) {
        source->active = false;
        source->is_socket = false;
        source->socket_event = NULL;
        (void)WSACloseEvent(event);
        return added;
    }
    return source->fd;
}
#endif

int inkwell_loop_watch_socket(struct inkwell_loop *loop, uintptr_t socket, uint32_t events,
                              inkwell_loop_callback callback, void *userdata, int *token) {
    if (token == NULL) {
        return -EINVAL;
    }
    *token = -1;
#if defined(_WIN32)
    const int added = inkwell_loop_add_socket(loop, socket, events, callback, userdata);
    if (added < 0) {
        return added;
    }
    *token = added;
#else
    if (socket > INT_MAX) {
        return -EINVAL;
    }
    const int fd = (int)socket;
    const int added = inkwell_loop_add_fd(loop, fd, events, callback, userdata);
    if (added < 0) {
        return added;
    }
    *token = fd;
#endif
    return 0;
}

int inkwell_loop_update_fd(struct inkwell_loop *loop, int fd, uint32_t events) {
    if (loop == NULL) {
        return -EINVAL;
    }

    int index = find_source_index(loop, fd);
    if (index < 0) {
        return -ENOENT;
    }

    struct inkwell_loop_source *source = &loop->sources[index];
    const uint32_t was = source->events;
    source->events = events;

    const int updated = backend_update(loop, source, was);
    if (updated < 0) {
        source->events = was;
        inkwell_log_error("loop", "changing fd %d failed: %s", fd, strerror(-updated));
        return updated;
    }

    return 0;
}

int inkwell_loop_remove_fd(struct inkwell_loop *loop, int fd) {
    if (loop == NULL) {
        return -EINVAL;
    }

    int index = find_source_index(loop, fd);
    if (index < 0) {
        return -ENOENT;
    }

    const int removed = backend_remove(loop, &loop->sources[index]);
    if (removed < 0) {
        inkwell_log_error("loop", "unwatching fd %d failed: %s", fd, strerror(-removed));
        return removed;
    }

    loop->sources[index].active = false;
    loop->sources[index].fd = -1;
    loop->sources[index].callback = NULL;
    loop->sources[index].userdata = NULL;
    loop->sources[index].events = 0;
#if defined(_WIN32)
    loop->sources[index].socket = 0U;
    loop->sources[index].socket_event = NULL;
    loop->sources[index].is_socket = false;
#endif

    return 0;
}

/*
 * One wait and everything it returned, dispatched. Returns how many sources were ready - 0 on a
 * timeout - or a negative errno, -EINTR included, which the caller retries.
 */
#if defined(_WIN32)
static uint32_t socket_ready(struct inkwell_loop_source *source) {
    WSANETWORKEVENTS reported;
    if (WSAEnumNetworkEvents((SOCKET)source->socket, (WSAEVENT)source->socket_event, &reported) ==
        SOCKET_ERROR) {
        return INKWELL_LOOP_ERR;
    }
    uint32_t mask = 0U;
    if ((reported.lNetworkEvents & (FD_READ | FD_ACCEPT)) != 0) {
        mask |= INKWELL_LOOP_IN;
    }
    if ((reported.lNetworkEvents & (FD_WRITE | FD_CONNECT)) != 0) {
        mask |= INKWELL_LOOP_OUT;
    }
    if ((reported.lNetworkEvents & FD_CLOSE) != 0) {
        mask |= INKWELL_LOOP_IN | INKWELL_LOOP_HUP;
    }
    const struct {
        long event;
        int bit;
    } errors[] = {{FD_READ, FD_READ_BIT},
                  {FD_ACCEPT, FD_ACCEPT_BIT},
                  {FD_WRITE, FD_WRITE_BIT},
                  {FD_CONNECT, FD_CONNECT_BIT},
                  {FD_CLOSE, FD_CLOSE_BIT}};
    for (size_t i = 0U; i < sizeof errors / sizeof errors[0]; ++i) {
        if ((reported.lNetworkEvents & errors[i].event) != 0 &&
            reported.iErrorCode[errors[i].bit] != 0) {
            mask |= INKWELL_LOOP_ERR;
        }
    }
    return mask;
}

static int backend_dispatch(struct inkwell_loop *loop, int wait_ms) {
    HANDLE handles[INKWELL_LOOP_MAX_SOURCES];
    struct inkwell_loop_source *sources[INKWELL_LOOP_MAX_SOURCES];
    DWORD count = 0;
    for (int i = 0; i < INKWELL_LOOP_MAX_SOURCES; ++i) {
        if (!loop->sources[i].active) {
            continue;
        }
        HANDLE handle = loop->sources[i].is_socket
                            ? (HANDLE)loop->sources[i].socket_event
                            : inkwell_windows_handle_get(loop->sources[i].fd);
        if (handle == NULL) {
            return -EBADF;
        }
        handles[count] = handle;
        sources[count] = &loop->sources[i];
        ++count;
    }
    if (count == 0U) {
        if (wait_ms < 0) {
            return -EINVAL;
        }
        Sleep((DWORD)wait_ms);
        return 0;
    }

    const DWORD timeout = wait_ms < 0 ? INFINITE : (DWORD)wait_ms;
    const DWORD first = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (first == WAIT_TIMEOUT) {
        return 0;
    }
    if (first >= WAIT_OBJECT_0 + count) {
        return -EIO;
    }

    int ready = 0;
    const DWORD first_index = first - WAIT_OBJECT_0;
    for (DWORD pass = 0; pass < count; ++pass) {
        const DWORD index = pass == 0U ? first_index : pass - (pass <= first_index ? 1U : 0U);
        struct inkwell_loop_source *source = sources[index];
        if (!source->active) {
            continue;
        }
        HANDLE current = source->is_socket ? (HANDLE)source->socket_event
                                           : inkwell_windows_handle_get(source->fd);
        if (current != handles[index] ||
            (pass > 0U && WaitForSingleObject(handles[index], 0) != WAIT_OBJECT_0)) {
            continue;
        }
        const uint32_t events = source->is_socket ? socket_ready(source) : source->events;
        if (events != 0U) {
            source->callback(source->fd, events, source->userdata);
            ++ready;
        }
    }
    return ready;
}
#elif defined(__linux__)
static int backend_dispatch(struct inkwell_loop *loop, int wait_ms) {
    struct epoll_event events[8];
    const int ready =
        epoll_wait(loop->poll_fd, events, (int)(sizeof events / sizeof events[0]), wait_ms);
    if (ready < 0) {
        return -errno;
    }
    for (int i = 0; i < ready; ++i) {
        struct inkwell_loop_source *source = (struct inkwell_loop_source *)events[i].data.ptr;
        if (source == NULL || !source->active) {
            continue;
        }
        source->callback(source->fd, events[i].events, source->userdata);
    }
    return ready;
}
#else
static int backend_dispatch(struct inkwell_loop *loop, int wait_ms) {
    struct kevent events[16];
    struct timespec timeout;
    struct timespec *wait = NULL;
    if (wait_ms >= 0) {
        timeout.tv_sec = wait_ms / 1000;
        timeout.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
        wait = &timeout;
    }
    const int got =
        kevent(loop->poll_fd, NULL, 0, events, (int)(sizeof events / sizeof events[0]), wait);
    if (got < 0) {
        return -errno;
    }

    /* Merged first, dispatched second: the one-callback-per-source-per-wait that epoll gives. The
       descriptor is kept beside the source so a slot freed and refilled by an earlier callback
       in this batch is not handed an event that was about its predecessor. */
    struct {
        struct inkwell_loop_source *source;
        int fd;
        uint32_t mask;
    } ready[16];
    int count = 0;
    for (int i = 0; i < got; ++i) {
        struct inkwell_loop_source *source = (struct inkwell_loop_source *)events[i].udata;
        if (source == NULL) {
            continue;
        }
        int slot = 0;
        while (slot < count && ready[slot].source != source) {
            ++slot;
        }
        if (slot == count) {
            ready[count].source = source;
            ready[count].fd = (int)events[i].ident;
            ready[count].mask = 0U;
            ++count;
        }
        ready[slot].mask |= kqueue_mask(&events[i]);
    }

    for (int i = 0; i < count; ++i) {
        struct inkwell_loop_source *source = ready[i].source;
        if (!source->active || source->fd != ready[i].fd) {
            continue;
        }
        source->callback(source->fd, ready[i].mask, source->userdata);
    }
    return count;
}
#endif

int inkwell_loop_run(struct inkwell_loop *loop, int timeout_ms) {
    if (loop == NULL) {
        return -EINVAL;
    }

    loop->running = true;
    loop->stop_requested = false;

    /*
     * `timeout_ms` bounds the whole call, not each wait.
     *
     * Returning only on an idle epoll made the caller's periodic work - a transport tick, a UI
     * publish, an update deadline - conditional on the loop going quiet, and any fd that
     * re-arms itself faster than that is enough to make sure it never does. A screen progress
     * bar is exactly that fd: it re-arms the 33 ms frame timer for as long as it is drawn, and
     * it is drawn for as long as the work it is reporting is unfinished - which is work that
     * only happens in the tick this call was starving. An application whose peer went quiet
     * mid-sync then sat there animating a bar about a sync that could not advance, forever.
     *
     * So the deadline is checked after dispatching rather than before: a caller that asked for
     * zero still drains what is already ready, exactly as it did, and one that asked for a
     * timeout now gets control back within it whether the loop fell idle or not.
     */
    const bool bounded = timeout_ms > 0;
    const uint64_t deadline_ms = bounded ? inkwell_time_monotonic_ms() + (uint64_t)timeout_ms : 0U;

    while (loop->running) {
        int wait_ms = timeout_ms;
        if (bounded) {
            const uint64_t now = inkwell_time_monotonic_ms();
            wait_ms = now >= deadline_ms ? 0 : (int)(deadline_ms - now);
        }
        int ready = backend_dispatch(loop, wait_ms);
        if (ready < 0) {
            if (ready == -EINTR) {
                continue;
            }
            inkwell_log_error("loop", "waiting failed: %s", strerror(-ready));
            loop->running = false;
            return ready;
        }

        if (ready == 0) {
            // Timeout without events; allow caller to regain control.
            break;
        }

        if (bounded && inkwell_time_monotonic_ms() >= deadline_ms) {
            break;
        }
    }

    loop->running = false;
    return 0;
}

void inkwell_loop_request_stop(struct inkwell_loop *loop) {
    if (loop == NULL) {
        return;
    }

    loop->running = false;
    loop->stop_requested = true;
    if (loop->wake.write_fd >= 0) {
        const int signalled = inkwell_wake_signal(&loop->wake);
        if (signalled < 0) {
            inkwell_log_warn("loop", "wake write failed: %s", strerror(-signalled));
        }
    }
}
