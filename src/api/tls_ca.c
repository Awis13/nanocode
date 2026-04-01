/*
 * tls_ca.c — system CA bundle loader for BearSSL (CMP-145)
 *
 * Reads a PEM CA bundle from the first path that exists, then walks every
 * CERTIFICATE block with BearSSL's PEM decoder + X.509 decoder to extract
 * DN and public-key data into br_x509_trust_anchor structs.
 *
 * Memory layout per anchor:
 *   ta.dn.data       — malloc'd copy of the subject DN
 *   ta.pkey.key.rsa.n/e  or  ta.pkey.key.ec.q  — malloc'd copies
 *
 * tls_ca_free() releases everything.
 */

#include "tls_ca.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * System CA bundle paths — tried in order, first match wins.
 * ---------------------------------------------------------------------- */

static const char * const ca_paths[] = {
    "/etc/ssl/cert.pem",                      /* macOS, FreeBSD           */
    "/etc/ssl/certs/ca-certificates.crt",     /* Debian / Ubuntu          */
    "/etc/pki/tls/certs/ca-bundle.crt",       /* RHEL / CentOS / Fedora   */
    "/etc/ssl/ca-bundle.pem",                 /* SUSE                     */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* newer RHEL   */
    NULL
};

/* -------------------------------------------------------------------------
 * Growing byte buffer — used to accumulate DN and key material.
 * ---------------------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} ByteBuf;

static int bbuf_append(ByteBuf *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        while (ncap < b->len + n)
            ncap *= 2;
        unsigned char *tmp = realloc(b->data, ncap);
        if (!tmp)
            return -1;
        b->data = tmp;
        b->cap  = ncap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static void bbuf_free(ByteBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* BearSSL DN accumulator callback */
static void dn_append_cb(void *ctx, const void *buf, size_t len)
{
    bbuf_append((ByteBuf *)ctx, buf, len);
}

/* BearSSL PEM decoder destination callback — void return required */
static void der_append_cb(void *ctx, const void *data, size_t len)
{
    bbuf_append((ByteBuf *)ctx, data, len);
}

/* -------------------------------------------------------------------------
 * Growing trust-anchor array.
 * ---------------------------------------------------------------------- */

typedef struct {
    br_x509_trust_anchor *data;
    size_t                len;
    size_t                cap;
} TAVec;

static int tavec_push(TAVec *v, const br_x509_trust_anchor *ta)
{
    if (v->len >= v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        br_x509_trust_anchor *tmp =
            realloc(v->data, ncap * sizeof(*tmp));
        if (!tmp)
            return -1;
        v->data = tmp;
        v->cap  = ncap;
    }
    v->data[v->len++] = *ta;
    return 0;
}

/* -------------------------------------------------------------------------
 * Convert one DER-encoded certificate into a br_x509_trust_anchor.
 * Returns 0 on success, -1 on error (partial cleanup handled internally).
 * ---------------------------------------------------------------------- */

static int der_to_ta(const unsigned char *der, size_t der_len,
                     br_x509_trust_anchor *ta_out)
{
    br_x509_decoder_context dc;
    ByteBuf dn = {0};

    br_x509_decoder_init(&dc, dn_append_cb, &dn);
    br_x509_decoder_push(&dc, der, der_len);

    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) {
        bbuf_free(&dn);
        return -1;
    }

    ta_out->dn.data = dn.data; /* transfer ownership */
    ta_out->dn.len  = dn.len;
    ta_out->flags   = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0u;

    switch (pk->key_type) {
    case BR_KEYTYPE_RSA: {
        unsigned char *n = malloc(pk->key.rsa.nlen);
        unsigned char *e = malloc(pk->key.rsa.elen);
        if (!n || !e) {
            free(n); free(e);
            free(ta_out->dn.data);
            ta_out->dn.data = NULL;
            return -1;
        }
        memcpy(n, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(e, pk->key.rsa.e, pk->key.rsa.elen);
        ta_out->pkey.key_type    = BR_KEYTYPE_RSA;
        ta_out->pkey.key.rsa.n   = n;
        ta_out->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta_out->pkey.key.rsa.e   = e;
        ta_out->pkey.key.rsa.elen = pk->key.rsa.elen;
        break;
    }
    case BR_KEYTYPE_EC: {
        unsigned char *q = malloc(pk->key.ec.qlen);
        if (!q) {
            free(ta_out->dn.data);
            ta_out->dn.data = NULL;
            return -1;
        }
        memcpy(q, pk->key.ec.q, pk->key.ec.qlen);
        ta_out->pkey.key_type     = BR_KEYTYPE_EC;
        ta_out->pkey.key.ec.curve = pk->key.ec.curve;
        ta_out->pkey.key.ec.q     = q;
        ta_out->pkey.key.ec.qlen  = pk->key.ec.qlen;
        break;
    }
    default:
        free(ta_out->dn.data);
        ta_out->dn.data = NULL;
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Parse a PEM bundle from memory; append decoded trust anchors to *tv.
 * Returns number of anchors appended.
 * ---------------------------------------------------------------------- */

static size_t parse_pem_bundle(const unsigned char *src, size_t src_len,
                               TAVec *tv)
{
    br_pem_decoder_context pdc;
    br_pem_decoder_init(&pdc);

    ByteBuf der     = {0};
    int     in_cert = 0;
    size_t  added   = 0;
    size_t  rem     = src_len;
    const unsigned char *p = src;

    /* BearSSL PEM decoder is fed in a loop; it stops at each object
     * boundary and we read the event before continuing. */
    while (rem > 0 || in_cert) {
        size_t consumed = br_pem_decoder_push(&pdc, p, rem);
        p   += consumed;
        rem -= consumed;

        int ev = br_pem_decoder_event(&pdc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            const char *name = br_pem_decoder_name(&pdc);
            if (strcmp(name, "CERTIFICATE") == 0 ||
                strcmp(name, "X509 CERTIFICATE") == 0) {
                der.len = 0; /* reuse buffer */
                br_pem_decoder_setdest(&pdc, der_append_cb, &der);
                in_cert = 1;
            } else {
                /* Ignore non-certificate PEM blocks */
                br_pem_decoder_setdest(&pdc, NULL, NULL);
                in_cert = 0;
            }
        } else if (ev == BR_PEM_END_OBJ) {
            if (in_cert && der.len > 0) {
                br_x509_trust_anchor ta;
                if (der_to_ta(der.data, der.len, &ta) == 0) {
                    if (tavec_push(tv, &ta) < 0) {
                        /* OOM: free the just-built anchor and stop */
                        tls_ca_free(&ta, 1);
                        break;
                    }
                    added++;
                }
            }
            in_cert = 0;
        } else if (ev == BR_PEM_ERROR) {
            break;
        } else {
            /* ev == 0 and rem == 0 means we need more input.
             * If rem == 0 we are done — add a trailing newline to flush
             * any incomplete final block (common in the wild). */
            if (rem == 0 && src[src_len - 1] != '\n') {
                static const unsigned char nl = '\n';
                p   = &nl;
                rem = 1;
                src_len++; /* prevent re-entry */
            } else {
                break;
            }
        }
    }

    bbuf_free(&der);
    return added;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

size_t tls_ca_load(br_x509_trust_anchor **anchors_out)
{
    *anchors_out = NULL;

    /* Find first readable CA bundle */
    const char *chosen = NULL;
    FILE *f = NULL;
    for (int i = 0; ca_paths[i]; i++) {
        f = fopen(ca_paths[i], "rb");
        if (f) {
            chosen = ca_paths[i];
            break;
        }
    }
    if (!f) {
        fprintf(stderr, "client: [DEBUG] no system CA bundle found; "
                        "falling back to no-verify TLS\n");
        return 0;
    }

    /* Read whole file */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long fsz = ftell(f);
    rewind(f);
    if (fsz <= 0) {
        fclose(f);
        return 0;
    }

    unsigned char *buf = malloc((size_t)fsz);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    if (got == 0) {
        free(buf);
        return 0;
    }

    /* Decode trust anchors */
    TAVec tv = {0};
    size_t n = parse_pem_bundle(buf, got, &tv);
    free(buf);

    if (n == 0) {
        free(tv.data);
        return 0;
    }

    fprintf(stderr, "client: [DEBUG] loaded %zu trust anchor(s) from %s\n",
            n, chosen);
    *anchors_out = tv.data;
    return n;
}

void tls_ca_free(br_x509_trust_anchor *anchors, size_t n)
{
    if (!anchors)
        return;
    for (size_t i = 0; i < n; i++) {
        free(anchors[i].dn.data);
        switch (anchors[i].pkey.key_type) {
        case BR_KEYTYPE_RSA:
            free(anchors[i].pkey.key.rsa.n);
            free(anchors[i].pkey.key.rsa.e);
            break;
        case BR_KEYTYPE_EC:
            free(anchors[i].pkey.key.ec.q);
            break;
        default:
            break;
        }
    }
    free(anchors);
}
