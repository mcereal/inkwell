#include "support/tls_identity.h"

/*
 * The server's certificate and key, generated for these fixtures and used nowhere else -
 * published here so a server can be stood up without a secret. Self-signed with CA:TRUE, so the
 * one PEM is both what the server presents and the whole bundle a client trusts; valid to 2126.
 *
 * The names on it are real ones on purpose. A case points a real URL at a fixture with
 * `inkwell_fetch_connect_to()`, and what the certificate is then checked against is the name in
 * the URL - so a certificate issued to "localhost" would make every case test the wrong thing.
 * It also carries 127.0.0.1 as an address, for a peer reached by number.
 */
static const char k_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIB9zCCAZ2gAwIBAgIUJ/M9e1NCKBJ7l1z5T0WZlBV9oGAwCgYIKoZIzj0EAwIw\n"
    "HjEcMBoGA1UEAwwTaW5rd2VsbC10ZXN0LXNlcnZlcjAgFw0yNjA5MjExNjQwMjRa\n"
    "GA8yMTI2MDgyODE2NDAyNFowHjEcMBoGA1UEAwwTaW5rd2VsbC10ZXN0LXNlcnZl\n"
    "cjBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABLxIp7jVCe9WR4YQAYlBpKRDjeJg\n"
    "ZH3phsa5Hh5lyB5ai5v/XtunU/VuIs4VuN6mY+BrSDIOK+twExQ51zDBJbmjgbYw\n"
    "gbMwHQYDVR0OBBYEFHeIAhfXZwCNZrHdgtfI2cVekaM/MB8GA1UdIwQYMBaAFHeI\n"
    "AhfXZwCNZrHdgtfI2cVekaM/MA8GA1UdEwEB/wQFMAMBAf8wYAYDVR0RBFkwV4IJ\n"
    "bG9jYWxob3N0ggpnaXRodWIuY29tgg5hcGkuZ2l0aHViLmNvbYIXKi5naXRodWJ1\n"
    "c2VyY29udGVudC5jb22CD2V4YW1wbGUuaW52YWxpZIcEfwAAATAKBggqhkjOPQQD\n"
    "AgNIADBFAiEAzFXMyEwNJWwpbsuWwpDws1RVn3OHEsyo5yJdiRl5kc4CIETW21Z4\n"
    "PvQ3C3EKzGlifPIrMU5fJxoK95BaZqgEWK0B\n"
    "-----END CERTIFICATE-----\n";

static const char k_key_pem[] = "-----BEGIN PRIVATE KEY-----\n"
                                "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgW0Ugzcy/4MdU0lnW\n"
                                "AE8jfaFMVtYU+kD4KpGlzHhSBuKhRANCAAS8SKe41QnvVkeGEAGJQaSkQ43iYGR9\n"
                                "6YbGuR4eZcgeWoub/17bp1P1biLOFbjepmPga0gyDivrcBMUOdcwwSW5\n"
                                "-----END PRIVATE KEY-----\n";

/*
 * A second self-signed certificate, which no fixture presents and nothing here is signed by. It
 * exists to be a trust anchor that does not match: "the registered roots do not include this
 * server" is a different refusal from "there are no registered roots", and a case that cannot
 * tell them apart passes without testing either.
 */
static const char k_decoy_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBnDCCAUGgAwIBAgIUZKmF961ZkgNKOjD4Wfr2aNGHHi4wCgYIKoZIzj0EAwIw\n"
    "IjEgMB4GA1UEAwwXaW5rd2VsbCB0ZXN0IGRlY295IHJvb3QwIBcNMjYwOTIxMTYz\n"
    "OTM2WhgPMjEyNjA4MjgxNjM5MzZaMCIxIDAeBgNVBAMMF2lua3dlbGwgdGVzdCBk\n"
    "ZWNveSByb290MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAESkmU4lMKDbwxt97b\n"
    "SGOVa0Aot1IotrG1vZJno0YmhceJ+Rdmi96T9kZCeH8npioCu/hOqOyzsfZtFK1P\n"
    "P2VQSaNTMFEwHQYDVR0OBBYEFPjF6R+p6irZIpom/AOdJCDKxrQFMB8GA1UdIwQY\n"
    "MBaAFPjF6R+p6irZIpom/AOdJCDKxrQFMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZI\n"
    "zj0EAwIDSQAwRgIhAOLzy8k0BSu9f9N3Dvrhv2okKXqKDpPEAxI5w3QfvdsPAiEA\n"
    "pyMGPrJx7ztCH9CpBhXTSPQXImStbEzlEMOBg8/P0y4=\n"
    "-----END CERTIFICATE-----\n";

const char *tls_identity_cert_pem(void) {
    return k_cert_pem;
}

const char *tls_identity_key_pem(void) {
    return k_key_pem;
}

const char *tls_identity_decoy_pem(void) {
    return k_decoy_pem;
}
