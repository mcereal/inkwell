#pragma once

/*
 * The one server identity every TLS case presents, and the decoy that is not it.
 *
 * Kept apart from https_fixture.c, which forks and so is POSIX's alone, because a suite that
 * stands its peer up on its own thread's terms - net_tls_windows.c - needs the same certificate
 * rather than a second one to keep in step. See tls_identity.c for what the names on it are.
 */

/* The server's certificate, PEM. Self-signed with CA:TRUE, so it is also the whole bundle a
   client trusts. */
const char *tls_identity_cert_pem(void);

/* The private key behind it, PEM. */
const char *tls_identity_key_pem(void);

/* A self-signed certificate no server presents and nothing is signed by: a trust anchor that
   does not match. */
const char *tls_identity_decoy_pem(void);
