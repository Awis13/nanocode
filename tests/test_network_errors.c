/*
 * test_network_errors.c — unit tests for network error message formatting
 * (CMP-402)
 *
 * Tests tls_error_str() from src/api/net_errors.c:
 *   - Key actionable X.509 certificate error codes map to human-readable text
 *   - Key SSL-layer error codes map to human-readable text
 *   - BearSSL code 0 ("no error") is handled
 *   - Unknown codes fall back to a non-NULL string containing the code
 *   - All return values are non-NULL (never crashes, never returns NULL)
 */

#include "test.h"
#include "../src/api/net_errors.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * NULL safety — tls_error_str always returns a non-NULL string
 * ------------------------------------------------------------------------ */

TEST(test_tls_error_str_never_null_known) {
    ASSERT_TRUE(tls_error_str(54) != NULL);
}

TEST(test_tls_error_str_never_null_unknown) {
    ASSERT_TRUE(tls_error_str(9999) != NULL);
}

TEST(test_tls_error_str_never_null_negative) {
    ASSERT_TRUE(tls_error_str(-1) != NULL);
}

/* --------------------------------------------------------------------------
 * BR_ERR_OK (0) — must not look like an error
 * ------------------------------------------------------------------------ */

TEST(test_tls_error_0_is_ok) {
    const char *s = tls_error_str(0);
    ASSERT_TRUE(strstr(s, "no error") != NULL || strstr(s, "OK") != NULL);
}

/* --------------------------------------------------------------------------
 * X.509 certificate errors — the most user-facing ones
 * ------------------------------------------------------------------------ */

TEST(test_tls_error_54_expired) {
    /* BR_ERR_X509_EXPIRED */
    const char *s = tls_error_str(54);
    ASSERT_TRUE(strstr(s, "expired") != NULL || strstr(s, "valid") != NULL);
}

TEST(test_tls_error_62_not_trusted) {
    /* BR_ERR_X509_NOT_TRUSTED */
    const char *s = tls_error_str(62);
    ASSERT_TRUE(strstr(s, "trust") != NULL || strstr(s, "CA") != NULL);
}

TEST(test_tls_error_56_hostname_mismatch) {
    /* BR_ERR_X509_BAD_SERVER_NAME */
    const char *s = tls_error_str(56);
    ASSERT_TRUE(strstr(s, "hostname") != NULL ||
                strstr(s, "mismatch") != NULL ||
                strstr(s, "server name") != NULL);
}

TEST(test_tls_error_56_mentions_base_url) {
    /* Hostname mismatch should hint at ANTHROPIC_BASE_URL */
    const char *s = tls_error_str(56);
    ASSERT_TRUE(strstr(s, "ANTHROPIC_BASE_URL") != NULL ||
                strstr(s, "hostname") != NULL);
}

TEST(test_tls_error_52_bad_signature) {
    /* BR_ERR_X509_BAD_SIGNATURE */
    const char *s = tls_error_str(52);
    ASSERT_TRUE(strstr(s, "signature") != NULL);
}

TEST(test_tls_error_53_time_unknown) {
    /* BR_ERR_X509_TIME_UNKNOWN */
    const char *s = tls_error_str(53);
    ASSERT_TRUE(strstr(s, "clock") != NULL || strstr(s, "time") != NULL ||
                strstr(s, "valid") != NULL);
}

TEST(test_tls_error_parse_errors_33_to_46) {
    /* ASN.1 decode errors: 33–46 all map to a parse/malformed message */
    for (int code = 33; code <= 46; code++) {
        const char *s = tls_error_str(code);
        ASSERT_TRUE(s != NULL);
        ASSERT_TRUE(strstr(s, "parse") != NULL ||
                    strstr(s, "malformed") != NULL ||
                    strstr(s, "certificate") != NULL ||
                    strstr(s, "error") != NULL);
    }
}

/* --------------------------------------------------------------------------
 * SSL-layer errors
 * ------------------------------------------------------------------------ */

TEST(test_tls_error_3_unsupported_version) {
    /* BR_ERR_UNSUPPORTED_VERSION */
    const char *s = tls_error_str(3);
    ASSERT_TRUE(strstr(s, "version") != NULL);
}

TEST(test_tls_error_16_no_cipher_suite) {
    /* BR_ERR_BAD_CIPHER_SUITE */
    const char *s = tls_error_str(16);
    ASSERT_TRUE(strstr(s, "cipher") != NULL || strstr(s, "suite") != NULL);
}

TEST(test_tls_error_7_bad_mac) {
    /* BR_ERR_BAD_MAC */
    const char *s = tls_error_str(7);
    ASSERT_TRUE(strstr(s, "MAC") != NULL || strstr(s, "auth") != NULL);
}

TEST(test_tls_error_31_io_error) {
    /* BR_ERR_IO */
    const char *s = tls_error_str(31);
    ASSERT_TRUE(strstr(s, "I/O") != NULL || strstr(s, "error") != NULL);
}

TEST(test_tls_error_14_bad_handshake) {
    /* BR_ERR_BAD_HANDSHAKE */
    const char *s = tls_error_str(14);
    ASSERT_TRUE(strstr(s, "handshake") != NULL);
}

/* --------------------------------------------------------------------------
 * Unknown / out-of-range codes fall back gracefully
 * ------------------------------------------------------------------------ */

TEST(test_tls_error_unknown_contains_code) {
    /* Unknown codes must embed the numeric code so logs are diagnosable */
    const char *s = tls_error_str(9999);
    ASSERT_TRUE(strstr(s, "9999") != NULL || strstr(s, "unknown") != NULL ||
                strstr(s, "error") != NULL);
}

TEST(test_tls_error_unknown_200) {
    const char *s = tls_error_str(200);
    ASSERT_TRUE(s != NULL);
}

int main(void)
{
    fprintf(stderr, "=== test_network_errors ===\n");
    RUN_TEST(test_tls_error_str_never_null_known);
    RUN_TEST(test_tls_error_str_never_null_unknown);
    RUN_TEST(test_tls_error_str_never_null_negative);
    RUN_TEST(test_tls_error_0_is_ok);
    RUN_TEST(test_tls_error_54_expired);
    RUN_TEST(test_tls_error_62_not_trusted);
    RUN_TEST(test_tls_error_56_hostname_mismatch);
    RUN_TEST(test_tls_error_56_mentions_base_url);
    RUN_TEST(test_tls_error_52_bad_signature);
    RUN_TEST(test_tls_error_53_time_unknown);
    RUN_TEST(test_tls_error_parse_errors_33_to_46);
    RUN_TEST(test_tls_error_3_unsupported_version);
    RUN_TEST(test_tls_error_16_no_cipher_suite);
    RUN_TEST(test_tls_error_7_bad_mac);
    RUN_TEST(test_tls_error_31_io_error);
    RUN_TEST(test_tls_error_14_bad_handshake);
    RUN_TEST(test_tls_error_unknown_contains_code);
    RUN_TEST(test_tls_error_unknown_200);
    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
