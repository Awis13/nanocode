/*
 * net_errors.c — human-readable BearSSL error code mapping (CMP-402)
 *
 * BearSSL reports TLS errors as integer codes from two namespaces:
 *   0–31   : SSL-layer errors  (bearssl_ssl.h)
 *   32–62  : X.509/cert errors (bearssl_x509.h)
 *
 * This file maps the most actionable codes to plain-English messages with
 * hints about what the user can do.  All other codes fall through to a
 * generic message that includes the raw code for diagnostics.
 *
 * No BearSSL headers are included here — just integer literals — so tests
 * can link this file without pulling in the full TLS library.
 */

#include "net_errors.h"

#include <stdio.h>

/* Static buffer for the fallback "unknown error N" message. */
static char s_fallback[64];

const char *tls_error_str(int err)
{
    switch (err) {
    /* -----------------------------------------------------------------------
     * SSL-layer errors
     * -------------------------------------------------------------------- */
    case 0:   return "no error";
    case 1:   return "bad parameter";
    case 2:   return "bad state";
    case 3:   return "unsupported TLS version";
    case 4:   return "protocol version mismatch";
    case 5:   return "bad record length";
    case 6:   return "record too large";
    case 7:   return "message authentication failed (bad MAC)";
    case 8:   return "no random source available";
    case 9:   return "unknown record type";
    case 10:  return "unexpected message";
    case 12:  return "bad ChangeCipherSpec message";
    case 13:  return "bad alert message";
    case 14:  return "bad handshake message";
    case 15:  return "oversized session ID";
    case 16:  return "no shared cipher suite";
    case 17:  return "bad compression method";
    case 18:  return "bad fragmentation length";
    case 19:  return "bad secure renegotiation";
    case 20:  return "unexpected TLS extension";
    case 21:  return "server name indication error";
    case 22:  return "bad HelloDone message";
    case 23:  return "limit exceeded";
    case 24:  return "bad Finished message";
    case 25:  return "session resumption mismatch";
    case 26:  return "invalid algorithm";
    case 27:  return "invalid signature";
    case 28:  return "wrong key usage";
    case 29:  return "client authentication required";
    case 31:  return "I/O error during TLS handshake";

    /* -----------------------------------------------------------------------
     * X.509 certificate errors (32–62)
     * -------------------------------------------------------------------- */
    case 32:  return "certificate OK";

    /* ASN.1 / parse errors (33–46): group under a single message */
    case 33:  /* INVALID_VALUE    */
    case 34:  /* TRUNCATED        */
    case 35:  /* EMPTY_CHAIN      */
    case 36:  /* INNER_TRUNC      */
    case 37:  /* BAD_TAG_CLASS    */
    case 38:  /* BAD_TAG_VALUE    */
    case 39:  /* INDEFINITE_LEN   */
    case 40:  /* EXTRA_ELEMENT    */
    case 41:  /* UNEXPECTED       */
    case 42:  /* NOT_CONSTRUCTED  */
    case 43:  /* NOT_PRIMITIVE    */
    case 44:  /* PARTIAL_BYTE     */
    case 45:  /* BAD_BOOLEAN      */
    case 46:  return "certificate parse error (malformed certificate)";

    case 47:  return "certificate has a bad Distinguished Name";
    case 48:  return "certificate has a bad validity time field";
    case 49:  return "unsupported certificate feature";
    case 50:  return "certificate chain too long";
    case 51:  return "certificate uses a wrong key type";
    case 52:  return "certificate signature is invalid";
    case 53:  return "certificate validity period unknown (check system clock)";
    case 54:  return "certificate expired (or not yet valid — check system clock)";
    case 55:  return "certificate Distinguished Name mismatch";
    case 56:  return "hostname mismatch (check ANTHROPIC_BASE_URL)";
    case 57:  return "certificate has an unhandled critical extension";
    case 58:  return "certificate is not a valid CA certificate";
    case 59:  return "certificate has forbidden key usage";
    case 60:  return "server uses a weak public key";
    case 62:  return "certificate not trusted (unknown or untrusted CA)";

    default:
        snprintf(s_fallback, sizeof(s_fallback),
                 "unknown TLS error (code %d)", err);
        return s_fallback;
    }
}
