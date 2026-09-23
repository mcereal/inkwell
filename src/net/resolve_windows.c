#include "inkwell/net/resolve.h"

#include "inkwell/base/fd.h"
#include "inkwell/base/log.h"
#include "inkwell/runtime/loop.h"

#include "../runtime/windows_handle.h"

#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* winsock2.h must precede windows.h; windows_handle.h establishes that order. */
#include <ws2tcpip.h>

/* MinGW-w64's import library carries these Vista-era exports but its ws2tcpip.h currently omits
   their declarations. Keep the declarations beside the only backend that needs them. */
INT WSAAPI GetAddrInfoExOverlappedResult(LPOVERLAPPED overlapped);
INT WSAAPI GetAddrInfoExCancel(LPHANDLE handle);

struct resolve_windows_backend {
    OVERLAPPED overlapped;
    PADDRINFOEXW results;
    HANDLE cancel_handle;
    int event_token;
    wchar_t host[256];
    wchar_t service[8];
    ADDRINFOEXW hints;
    bool winsock_started;
    bool timed_out;
};

bool inkwell_resolve_literal(const char *host, uint16_t port, struct sockaddr_storage *out,
                             socklen_t *out_len) {
    return inkwell_socket_parse_literal(host, port, out, out_len);
}

static enum inkwell_resolve_outcome resolve_outcome_of(int error) {
    if (error == 0) {
        return INKWELL_RESOLVE_OK;
    }
    if (error == WSAHOST_NOT_FOUND || error == WSANO_DATA) {
        return INKWELL_RESOLVE_NOT_FOUND;
    }
    if (error == WSAETIMEDOUT) {
        return INKWELL_RESOLVE_TIMED_OUT;
    }
    return INKWELL_RESOLVE_FAILED;
}

static void resolve_pick(const ADDRINFOEXW *results, struct inkwell_resolve_result *result) {
    const ADDRINFOEXW *picked[INKWELL_RESOLVE_ADDRESSES_MAX] = {0};
    int family = results != NULL ? results->ai_family : AF_UNSPEC;

    while (result->address_count < INKWELL_RESOLVE_ADDRESSES_MAX) {
        const ADDRINFOEXW *pick = NULL;
        for (int pass = 0; pass < 2 && pick == NULL; ++pass) {
            for (const ADDRINFOEXW *candidate = results; candidate != NULL;
                 candidate = candidate->ai_next) {
                bool already_picked = false;
                for (size_t i = 0U; i < result->address_count; ++i) {
                    if (picked[i] == candidate) {
                        already_picked = true;
                        break;
                    }
                }
                if (!already_picked && candidate->ai_addr != NULL && candidate->ai_addrlen > 0U &&
                    candidate->ai_addrlen <= sizeof result->addresses[0].address &&
                    (pass == 1 || candidate->ai_family == family)) {
                    pick = candidate;
                    break;
                }
            }
        }
        if (pick == NULL) {
            break;
        }
        picked[result->address_count] = pick;
        result->addresses[result->address_count].len = (socklen_t)pick->ai_addrlen;
        memcpy(&result->addresses[result->address_count].address, pick->ai_addr, pick->ai_addrlen);
        result->address_count++;
        family = pick->ai_family == AF_INET6 ? AF_INET : AF_INET6;
    }

    if (result->address_count > 0U) {
        result->address = result->addresses[0].address;
        result->address_len = result->addresses[0].len;
    }
}

static void resolve_backend_release(struct resolve_windows_backend *backend) {
    if (backend == NULL) {
        return;
    }
    if (backend->event_token >= 0) {
        inkwell_windows_handle_close(backend->event_token);
    }
    if (backend->results != NULL) {
        FreeAddrInfoExW(backend->results);
    }
    if (backend->winsock_started) {
        (void)WSACleanup();
    }
    free(backend);
}

static void resolve_release(struct inkwell_resolve *resolve) {
    struct resolve_windows_backend *backend = (struct resolve_windows_backend *)resolve->backend;
    if (backend == NULL) {
        return;
    }
    if (backend->event_token >= 0 && resolve->loop != NULL) {
        (void)inkwell_loop_remove_fd(resolve->loop, backend->event_token);
    }
    resolve->backend = NULL;
    resolve_backend_release(backend);
}

static void resolve_complete(struct inkwell_resolve *resolve, int error) {
    struct resolve_windows_backend *backend = (struct resolve_windows_backend *)resolve->backend;
    if (backend == NULL) {
        return;
    }

    struct inkwell_resolve_result result;
    memset(&result, 0, sizeof result);
    result.error = error;
    result.outcome = backend->timed_out ? INKWELL_RESOLVE_TIMED_OUT : resolve_outcome_of(error);
    if (result.outcome == INKWELL_RESOLVE_OK) {
        resolve_pick(backend->results, &result);
        if (result.address_count == 0U) {
            result.outcome = INKWELL_RESOLVE_NOT_FOUND;
        }
    }

    const inkwell_resolve_done_fn done = resolve->on_done;
    void *const userdata = resolve->userdata;
    resolve_release(resolve);
    resolve->on_done = NULL;
    resolve->userdata = NULL;
    resolve->deadline_ms = 0U;
    if (done != NULL) {
        done(userdata, &result);
    }
}

static int resolve_on_ready(int fd, uint32_t events, void *userdata) {
    (void)fd;
    (void)events;
    struct inkwell_resolve *resolve = (struct inkwell_resolve *)userdata;
    if (resolve == NULL || resolve->backend == NULL) {
        return 0;
    }
    struct resolve_windows_backend *backend = (struct resolve_windows_backend *)resolve->backend;
    const int result = GetAddrInfoExOverlappedResult(&backend->overlapped);
    if (result != WSA_IO_INCOMPLETE) {
        resolve_complete(resolve, result);
    }
    return 0;
}

int inkwell_resolve_init(struct inkwell_resolve *resolve, struct inkwell_loop *loop) {
    if (resolve == NULL) {
        return -EINVAL;
    }
    memset(resolve, 0, sizeof *resolve);
    resolve->loop = loop;
    resolve->child = -1;
    resolve->child_fd = -1;
    return 0;
}

static void resolve_cancel_request(struct inkwell_resolve *resolve) {
    struct resolve_windows_backend *backend =
        resolve != NULL ? (struct resolve_windows_backend *)resolve->backend : NULL;
    if (backend == NULL) {
        return;
    }
    if (backend->cancel_handle != NULL) {
        (void)GetAddrInfoExCancel(&backend->cancel_handle);
    }
}

static unsigned __stdcall resolve_reap_cancelled(void *userdata) {
    struct resolve_windows_backend *backend = (struct resolve_windows_backend *)userdata;
    (void)WaitForSingleObject(backend->overlapped.hEvent, INFINITE);
    resolve_backend_release(backend);
    return 0U;
}

void inkwell_resolve_shutdown(struct inkwell_resolve *resolve) {
    inkwell_resolve_cancel(resolve);
}

bool inkwell_resolve_available(const struct inkwell_resolve *resolve) {
    return resolve != NULL && resolve->loop != NULL;
}

bool inkwell_resolve_busy(const struct inkwell_resolve *resolve) {
    return resolve != NULL && resolve->backend != NULL;
}

int inkwell_resolve_start(struct inkwell_resolve *resolve, const char *host, uint16_t port,
                          inkwell_resolve_done_fn on_done, void *userdata, uint64_t now_ms) {
    if (resolve == NULL || host == NULL || host[0] == '\0' || on_done == NULL) {
        return -EINVAL;
    }
    if (!inkwell_resolve_available(resolve)) {
        return -ENOTSUP;
    }
    if (resolve->backend != NULL) {
        return -EBUSY;
    }

    struct resolve_windows_backend *backend = calloc(1U, sizeof *backend);
    if (backend == NULL) {
        return -ENOMEM;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host, -1, backend->host,
                            (int)(sizeof backend->host / sizeof backend->host[0])) == 0 ||
        swprintf(backend->service, sizeof backend->service / sizeof backend->service[0], L"%u",
                 (unsigned)port) < 0) {
        free(backend);
        return -EINVAL;
    }
    backend->event_token = -1;
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        free(backend);
        return -EIO;
    }
    backend->winsock_started = true;
    backend->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (backend->overlapped.hEvent == NULL) {
        (void)WSACleanup();
        free(backend);
        return -EIO;
    }
    backend->event_token = inkwell_windows_handle_register(backend->overlapped.hEvent);
    if (backend->event_token < 0) {
        (void)CloseHandle(backend->overlapped.hEvent);
        const int error = backend->event_token;
        (void)WSACleanup();
        free(backend);
        return error;
    }

    backend->hints.ai_family = AF_UNSPEC;
    backend->hints.ai_socktype = SOCK_STREAM;
    backend->hints.ai_protocol = IPPROTO_TCP;
    backend->hints.ai_flags = AI_ADDRCONFIG;

    resolve->backend = backend;
    resolve->deadline_ms = now_ms + INKWELL_RESOLVE_TIMEOUT_MS;
    resolve->on_done = on_done;
    resolve->userdata = userdata;
    const int added = inkwell_loop_add_fd(resolve->loop, backend->event_token, INKWELL_LOOP_IN,
                                          resolve_on_ready, resolve);
    if (added != 0) {
        resolve_release(resolve);
        resolve->on_done = NULL;
        resolve->userdata = NULL;
        return added;
    }

    const int started = GetAddrInfoExW(backend->host, backend->service, NS_DNS, NULL,
                                       &backend->hints, &backend->results, NULL,
                                       &backend->overlapped, NULL, &backend->cancel_handle);
    if (started != 0 && started != WSA_IO_PENDING) {
        resolve_release(resolve);
        resolve->on_done = NULL;
        resolve->userdata = NULL;
        return started == WSA_NOT_ENOUGH_MEMORY ? -ENOMEM : -EIO;
    }
    if (started == 0) {
        (void)SetEvent(backend->overlapped.hEvent);
    }
    inkwell_log_debug("resolve", "Looking up %s:%u", host, (unsigned)port);
    return 0;
}

void inkwell_resolve_tick(struct inkwell_resolve *resolve, uint64_t now_ms) {
    if (resolve == NULL || resolve->backend == NULL || now_ms < resolve->deadline_ms) {
        return;
    }
    struct resolve_windows_backend *backend = (struct resolve_windows_backend *)resolve->backend;
    if (!backend->timed_out) {
        backend->timed_out = true;
        resolve_cancel_request(resolve);
    }
}

void inkwell_resolve_cancel(struct inkwell_resolve *resolve) {
    if (resolve == NULL || resolve->backend == NULL) {
        return;
    }
    struct resolve_windows_backend *backend = (struct resolve_windows_backend *)resolve->backend;
    resolve_cancel_request(resolve);
    if (backend->event_token >= 0 && resolve->loop != NULL) {
        (void)inkwell_loop_remove_fd(resolve->loop, backend->event_token);
    }
    resolve->backend = NULL;
    resolve->on_done = NULL;
    resolve->userdata = NULL;
    resolve->deadline_ms = 0U;

    const uintptr_t thread = _beginthreadex(NULL, 0U, resolve_reap_cancelled, backend, 0U, NULL);
    if (thread != 0U) {
        (void)CloseHandle((HANDLE)thread);
        return;
    }
    /* Resource exhaustion is already exceptional, and blocking the loop would make it worse.
       Keep the request storage alive for the OS rather than risking a use-after-free. */
    inkwell_log_error("resolve", "Could not start the cancelled-lookup reaper; leaking its state");
}
