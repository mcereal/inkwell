#include "inkwell/net/reason.h"

#include <errno.h>

enum inkwell_net_reason inkwell_net_reason_from_errno(int err) {
    /* Both spellings reach here: `errno` is positive and a returned error is negative. */
    if (err < 0) {
        err = -err;
    }
    switch (err) {
    case 0:
        return INKWELL_NET_OK;
    case ETIMEDOUT:
        return INKWELL_NET_TIMED_OUT;
    case ECONNRESET:
    case EPIPE:
        return INKWELL_NET_CLOSED;
    default:
        /*
         * ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EADDRNOTAVAIL, EACCES, EMFILE and the rest
         * all land here. That is not laziness about the rare ones: the question a caller is
         * answering is "can this host be reached", every one of these is no, and the number is
         * carried in `detail` for whoever wants to know which no it was.
         */
        return INKWELL_NET_UNREACHABLE;
    }
}

enum inkwell_net_reason inkwell_net_reason_from_resolve(enum inkwell_resolve_outcome outcome) {
    switch (outcome) {
    case INKWELL_RESOLVE_OK:
        return INKWELL_NET_OK;
    case INKWELL_RESOLVE_NOT_FOUND:
        return INKWELL_NET_UNKNOWN_HOST;
    case INKWELL_RESOLVE_TIMED_OUT:
        return INKWELL_NET_LOOKUP_TIMED_OUT;
    case INKWELL_RESOLVE_FAILED:
    case INKWELL_RESOLVE_OUTCOME_COUNT:
    default:
        return INKWELL_NET_LOOKUP_FAILED;
    }
}

const char *inkwell_net_reason_name(enum inkwell_net_reason reason) {
    switch (reason) {
    case INKWELL_NET_OK:
        return "ok";
    case INKWELL_NET_BAD_ADDRESS:
        return "bad-address";
    case INKWELL_NET_UNKNOWN_HOST:
        return "unknown-host";
    case INKWELL_NET_LOOKUP_FAILED:
        return "lookup-failed";
    case INKWELL_NET_LOOKUP_TIMED_OUT:
        return "lookup-timed-out";
    case INKWELL_NET_UNREACHABLE:
        return "unreachable";
    case INKWELL_NET_TIMED_OUT:
        return "timed-out";
    case INKWELL_NET_CLOSED:
        return "closed";
    case INKWELL_NET_TLS:
        return "tls";
    case INKWELL_NET_REASON_COUNT:
    default:
        break;
    }
    /* Never NULL: this is on a log path, and a log line is the last place to hand somebody a
       pointer they have to check. */
    return "unknown";
}
