#ifndef TLS_H
#define TLS_H

#include <stdbool.h>
#include <stddef.h>

// TLS for http.c, on top of the OpenSSL the device already ships.
//
// The rootfs carries /usr/lib/libssl.so.1.1 and libcrypto.so.1.1 (OpenSSL
// 1.1.1w from the assets, see tools/system-libs). The cross toolchain has no
// headers for them and nothing to link against, so they are opened at run time
// with dlopen() and the two dozen functions needed are looked up by name. That
// is the only way to use a library present on the target and absent from the
// build host, and it has a second virtue: a device whose rootfs lacks them
// still runs the player, it just cannot open an https:// station and says so.
//
// Verification is not optional: the certificate chain is checked against a CA
// bundle and the hostname against the certificate. A TLS connection that
// checks neither is a slower plain socket with a false sense of security, and
// the radio directory supplies URLs from strangers.
//
// Where the CA bundle is looked for, first hit wins:
//
//   1. [network] ca_bundle in device_config.ini -- an absolute path, for when
//      none of the entries below is the right place
//   2. <card>/.local/cacert.pem -- refreshable without reflashing anything,
//      which matters because certificates expire and firmware does not
//   3. /etc/ssl/cert.pem and /etc/ssl/certs/ -- where this OpenSSL looks by
//      default (its OPENSSLDIR is "/etc/ssl"), so a bundle dropped there by a
//      firmware repack is found with no configuration at all
//
// The bundle to use is Mozilla's, which is what a browser trusts. curl
// publishes it, extracted from Firefox's own store, at
// https://curl.se/ca/cacert.pem.

// True when libssl and libcrypto were found and every symbol resolved. Cheap
// to call repeatedly; the loading happens once.
bool tls_available(void);

// What OpenSSL says it is, for the log. Empty when unavailable.
const char *tls_version(void);

// Where the CA bundle was actually loaded from, or NULL when none was found.
// Worth logging once: "cannot open https" and "found no certificates" are very
// different problems with identical symptoms.
const char *tls_ca_source(void);

// The card's root, so <card>/.local/cacert.pem can be looked for. Call once at
// startup, before the first connection. NULL or unset simply drops that entry
// from the search.
void tls_set_card_root(const char *sd_root);

typedef struct tls_conn tls_conn_t;

// Wraps an already-connected socket in TLS and performs the handshake,
// including certificate and hostname verification. `hostname` is both the SNI
// sent and the name checked against the certificate, so it must be the name
// from the URL and never an address the resolver produced.
//
// Returns NULL on failure, with a human-readable reason in `err` -- meant to
// be shown to the user, so it is in the same language as the rest of the
// player. The socket is left alone: the caller still owns and closes it.
tls_conn_t *tls_client(int fd, const char *hostname, char *err, size_t err_size);

// Same shapes as recv()/send(): >0 bytes moved, 0 for a clean end of stream,
// -1 on error (a socket timeout included).
int tls_read(tls_conn_t *c, void *buf, int len);
int tls_write(tls_conn_t *c, const void *buf, int len);

// Drops the TLS session. Does not close the socket.
void tls_free(tls_conn_t *c);

#endif /* TLS_H */
