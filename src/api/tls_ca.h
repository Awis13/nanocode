/*
 * tls_ca.h — system CA bundle loader for BearSSL (CMP-145)
 *
 * Probes standard CA bundle locations on macOS and Linux and returns a
 * malloc-owned array of br_x509_trust_anchor suitable for use with
 * br_ssl_client_init_full().
 */

#ifndef TLS_CA_H
#define TLS_CA_H

#include <stddef.h>
#include "bearssl.h"

/*
 * Probe standard CA bundle paths and load trust anchors from the first
 * one found.  On success, *anchors_out is set to a malloc-allocated array
 * of num_anchors entries, each holding its own malloc-allocated key data.
 *
 * Returns the number of anchors loaded (>= 1) or 0 on failure (no bundle
 * found, parse error, or OOM).  Logs the chosen path at DEBUG level to
 * stderr.
 *
 * Caller must release the array with tls_ca_free() when done.
 */
size_t tls_ca_load(br_x509_trust_anchor **anchors_out);

/*
 * Release an anchor array previously returned by tls_ca_load().
 * Safe to call with anchors == NULL or n == 0.
 */
void tls_ca_free(br_x509_trust_anchor *anchors, size_t n);

#endif /* TLS_CA_H */
