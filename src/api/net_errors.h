/*
 * net_errors.h — human-readable network error strings (CMP-402)
 *
 * Provides tls_error_str() which maps BearSSL integer error codes to
 * actionable, user-facing messages — no BearSSL headers required by callers.
 */

#ifndef NET_ERRORS_H
#define NET_ERRORS_H

/*
 * Return a human-readable string for a BearSSL engine last_error code.
 * The returned pointer is always non-NULL (never crashes, never returns NULL).
 * The string is suitable for printing directly to stderr.
 */
const char *tls_error_str(int err);

#endif /* NET_ERRORS_H */
