#pragma once

#include "inkwell/base/fd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A TLS client session over a socket somebody else owns.
 *
 * The two shapes it is built for are a long-lived bidirectional session and a single large
 * transfer - net/fetch.c is the second. Both have to sit on the same event loop as everything
 * else and stay readable and writable between whatever else that loop is doing, which on a
 * device with a screen is drawing it. There is nowhere to block.
 *
 * So this is Mbed TLS (third_party/mbedtls, a pinned submodule) driven through BIO callbacks
 * over a non-blocking socket, reporting `-EAGAIN` all the way up rather than waiting. The
 * library is configured by the pair of files in third_party/mbedtls-config/, which say what was
 * taken out of it and why - `inkwell_mbedtls_config.h` for TLS and X.509,
 * `inkwell_psa_crypto_config.h`
 * for everything cryptographic, because 4.x is two projects.
 *
 * **Certificates are always verified.** There is no insecure mode and no argument that turns one
 * on. A device with no system certificate store is what makes one tempting - but a connection
 * carries this client's traffic to somebody else's server, and an unauthenticated one is a
 * connection to whoever answered.
 *
 * *What* they are verified against is not decided here. `inkwell_tls_set_roots()` is handed the
 * trust anchors once at startup and every session is checked against those; where they came from
 * - compiled in, read off a card, taken from a system store - is a question about the product,
 * not about TLS. An application that ships on a device with no certificate store may well
 * compile a public root set into its binary; one on a general-purpose machine would rather read
 * the system's. Both are the same call from down here, and neither is this file's to make.
 *
 * A build without the submodule compiles this to a stub that reports itself unavailable and
 * refuses to start, the same way the BLE transport compiles out without D-Bus headers. Nothing
 * that uses it needs an #ifdef.
 */

/* The socket is borrowed, never closed here: the caller opened it, the caller connected it,
   and on any error the caller is the one that has to decide whether to retry. */
struct inkwell_tls_state; /* defined in tls.c; heap-held, one per session */

struct inkwell_tls_client {
    inkwell_socket socket;
    struct inkwell_tls_state *state; /* NULL until start(), and again after stop() */
    /*
     * Which direction the session is blocked on, which is *not* a property of what the caller
     * asked for. A handshake flight, and a write that triggers one, can block waiting to read;
     * a read can block waiting to write. Arming the wrong loop event parks the connection
     * forever on a socket that will never become ready in the direction being watched.
     */
    bool wants_write;
    /*
     * Set when read() gave the loop back with work still inside the session rather than because
     * the socket was empty - today, a run of session tickets long enough to hit its budget. The
     * socket may well have nothing to report, so a caller that waits for the loop after this
     * waits forever; it has to come back of its own accord on its next turn. A caller that
     * keeps a per-turn read budget of its own wants the same flag for the same reason, and
     * net/fetch.c carries one. Cleared at the top of every read.
     */
    bool more_to_read;
    char error[160];
    /* The library's own negative code behind `error`, or 0 - see inkwell_tls_client_error_code().
     */
    int error_code;
};

/* True when this build has Mbed TLS at all. False makes every start() fail with -ENOTSUP. */
bool inkwell_tls_available(void);

/* One trust anchor: the DER, and the label whoever supplied it gives it - which is what a log
   line has to name when a root will not parse, since the DER itself says nothing a reader can
   use. */
struct inkwell_tls_ca_root {
    const char *name;
    const unsigned char *der;
    size_t len;
};

/*
 * The trust anchors every session is verified against. Called once, before the first session,
 * from wherever this process decides what it trusts.
 *
 * `roots` is borrowed and must be static for the life of the process: the certificates are parsed
 * without copying, so the chain Mbed TLS walks points into this table rather than into the heap.
 * Calling it again replaces the set and discards what was parsed from the last one.
 *
 * There is no default and no fallback. With nothing registered, a session that names no bundle
 * file fails at start() rather than connecting to something it cannot check - because a layer
 * this far down has no business holding an opinion about who a product trusts, and the failure
 * mode of guessing is a connection that looks fine.
 */
void inkwell_tls_set_roots(const struct inkwell_tls_ca_root *roots, size_t count);

/*
 * The bundle file somebody asked for in place of the registered roots - `SSL_CERT_FILE`, the
 * variable OpenSSL and curl already honour - or NULL for none. For a server behind a private CA,
 * and deliberately nothing more clever: a path that is set is used, readable or not, so that a
 * typo fails naming the file rather than falling back and failing somewhere less obvious.
 */
const char *inkwell_tls_ca_override(void);

/*
 * Begins a session on `socket`, which must already be open, non-blocking and connected (or
 * connecting - the first handshake flight will simply block until it is).
 *
 * `hostname` is both the SNI to send and the name the certificate is checked against, so it is
 * the name the user typed rather than the address it resolved to: a certificate is issued for a
 * name, and checking one against an IP address fails against very nearly every server there is.
 *
 * `ca_bundle` is a PEM file to verify against instead of the registered roots, or NULL/"" for the
 * registered roots. A named file that is missing or holds nothing usable is a hard failure, never
 * a fallback - see inkwell_tls_ca_override(). Returns 0, -ENOTSUP without Mbed TLS, -EINVAL for bad
 * arguments, -EIO when the library or the bundle would not initialise, or -EIO with no roots
 * registered and no bundle named, with error() set.
 */
int inkwell_tls_client_start(struct inkwell_tls_client *tls, inkwell_socket socket,
                             const char *hostname, const char *ca_bundle);

/*
 * Drives the handshake. Returns 0 when it is complete, -EAGAIN when it needs the socket to
 * become ready again (check wants_write for which way), or a negative errno on a failure that
 * will not improve - a bad certificate, a name that does not match, no shared cipher suite.
 *
 * Call it once per readiness event until it stops saying -EAGAIN. It is safe to call after it
 * has returned 0, where it returns 0 again.
 */
int inkwell_tls_client_handshake(struct inkwell_tls_client *tls);

/*
 * Reads decrypted bytes. Returns the count, 0 never, -EAGAIN when there is nothing ready,
 * -ENOTCONN when the peer closed the session cleanly, or another negative errno.
 *
 * **A reader must loop until -EAGAIN.** One TLS record can hold more than one call's worth of
 * plaintext, and the leftovers live inside the session rather than in the socket - so the
 * socket is empty, the loop has nothing to report, and a reader that stops after one call waits
 * forever on data it has already received. This is the classic way TLS on an event loop hangs.
 *
 * **A -EAGAIN with `more_to_read` set is not an empty socket** and must not be answered by
 * waiting for one. It means this stopped early to give the loop back, and the caller has to
 * call again on its next turn.
 */
int inkwell_tls_client_read(struct inkwell_tls_client *tls, uint8_t *out, size_t cap);

/*
 * Writes plaintext. Returns the count written, which may be short, -EAGAIN when the socket
 * would block, or another negative errno.
 *
 * A short write must be retried with the same bytes at the same offset: Mbed TLS has already
 * framed them into a record and will refuse a caller that comes back with something else.
 */
int inkwell_tls_client_write(struct inkwell_tls_client *tls, const uint8_t *data, size_t len);

/*
 * Ends the session and releases everything it held. Does **not** close the socket.
 *
 * No close_notify is sent. It would be one more write that can block on a socket the caller is
 * about to close anyway, and the thing it protects against - a truncation attack on a stream
 * whose end is meaningful - is the *peer's* to worry about. A protocol that frames its own
 * messages already knows where each one ends, and a caller reading until close is owed the
 * server's close_notify rather than its own; net/fetch.c is the second case, and treats a body
 * that ended without one as truncated.
 */
void inkwell_tls_client_stop(struct inkwell_tls_client *tls);

/* Why the last call failed, in words, or "" when none has. Never NULL. */
const char *inkwell_tls_client_error(const struct inkwell_tls_client *tls);

/*
 * The TLS library's own code behind inkwell_tls_client_error() - negative, and only meaningful
 * to someone with the library's headers open - or 0 when the last failure had none: nothing has
 * failed, the peer closed cleanly, or the failure was this file's (a root that would not load)
 * rather than the library's. It is what `detail` carries for INKWELL_NET_TLS in net/reason.h:
 * worth logging, never worth showing.
 */
int inkwell_tls_client_error_code(const struct inkwell_tls_client *tls);

#ifdef __cplusplus
}
#endif
