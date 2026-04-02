/*
 * sandbox.c — OS-level sandbox enforcement (CMP-200)
 *
 * Maps SandboxConfig into OS kernel policies:
 *   macOS : sandbox-exec SBPL deny-all policy via sandbox_init(3)
 *   Linux : Landlock LSM ruleset (kernel 5.13+)
 *
 * No dynamic allocation.  SBPL policy is built in a 4 KB stack buffer.
 * Call sequence: sandbox_config_from_cfg → sandbox_validate → sandbox_activate
 */

#include "../../include/sandbox.h"
#include "../../include/config.h"

#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#include <stdint.h>
/* Declare privately to avoid deprecation attribute from <sandbox.h>. */
extern int  sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
extern void sandbox_free_error(char *errorbuf);
#endif /* __APPLE__ */

#ifdef __linux__
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/prctl.h>

/* Landlock syscall numbers (x86-64 / arm64 / riscv64 share 444-446). */
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule       445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self  446
#endif

/* Landlock ABI structs — defined locally for portability on older distros. */
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)

struct ll_ruleset_attr {
    uint64_t handled_access_fs;
};

struct ll_path_beneath_attr {
    uint64_t allowed_access;
    int32_t  parent_fd;
} __attribute__((packed));

/* Filesystem access rights (ABI 1, kernel 5.13+). */
#define LL_FS_READ  ( \
    (1ULL <<  0) |  /* EXECUTE       */ \
    (1ULL <<  1) |  /* WRITE_FILE    */ \
    (1ULL <<  2) |  /* READ_FILE     */ \
    (1ULL <<  3) |  /* READ_DIR      */ \
    (1ULL <<  4) |  /* REMOVE_DIR    */ \
    (1ULL <<  5) |  /* REMOVE_FILE   */ \
    (1ULL <<  6) |  /* MAKE_CHAR     */ \
    (1ULL <<  7) |  /* MAKE_DIR      */ \
    (1ULL <<  8) |  /* MAKE_REG      */ \
    (1ULL <<  9) |  /* MAKE_SOCK     */ \
    (1ULL << 10) |  /* MAKE_FIFO     */ \
    (1ULL << 11) |  /* MAKE_BLOCK    */ \
    (1ULL << 12))   /* MAKE_SYM      */

static inline int ll_create_ruleset(const struct ll_ruleset_attr *attr,
                                    size_t size, uint32_t flags)
{
    return (int)syscall(__NR_landlock_create_ruleset, attr, size, flags);
}

static inline int ll_add_rule(int ruleset_fd, uint32_t rule_type,
                               const void *rule_attr, uint32_t flags)
{
    return (int)syscall(__NR_landlock_add_rule,
                        ruleset_fd, rule_type, rule_attr, flags);
}

static inline int ll_restrict_self(int ruleset_fd, uint32_t flags)
{
    return (int)syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}

#define LANDLOCK_RULE_PATH_BENEATH 1
#endif /* LANDLOCK_CREATE_RULESET_VERSION */

#endif /* __linux__ */

/* -------------------------------------------------------------------------
 * Internal: iterate colon-separated path segments
 *
 * Copies the next segment from *pos into buf[bufsz].
 * Advances *pos past the separator.
 * Returns 1 if a segment was copied, 0 when the string is exhausted.
 * ---------------------------------------------------------------------- */
static int next_segment(const char **pos, char *buf, size_t bufsz)
{
    if (!pos || !*pos || **pos == '\0')
        return 0;

    const char *start = *pos;
    const char *end   = strchr(start, ':');
    size_t      len;

    if (end) {
        len  = (size_t)(end - start);
        *pos = end + 1;
    } else {
        len  = strlen(start);
        *pos = start + len;
    }

    if (len == 0)
        return 0;

    if (len >= bufsz)
        len = bufsz - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return 1;
}

/* =========================================================================
 * Public: sandbox_config_from_cfg
 * ====================================================================== */

void sandbox_config_from_cfg(SandboxConfig *sc, const Config *cfg)
{
    if (!sc || !cfg) return;
    sc->enabled          = config_get_bool(cfg, "sandbox.enabled");
    sc->profile          = config_get_str (cfg, "sandbox.profile");
    sc->allowed_paths    = config_get_str (cfg, "sandbox.allowed_paths");
    sc->allowed_commands = config_get_str (cfg, "sandbox.allowed_commands");
    sc->network          = config_get_bool(cfg, "sandbox.network");
    sc->max_file_size    = (long)config_get_int(cfg, "sandbox.max_file_size");
}

/* =========================================================================
 * Public: sandbox_validate
 * ====================================================================== */

int sandbox_validate(const SandboxConfig *sc)
{
    if (!sc) {
        fprintf(stderr, "sandbox: validate called with NULL config\n");
        return -1;
    }

    if (!sc->profile || sc->profile[0] == '\0') {
        fprintf(stderr, "sandbox: profile must be set\n");
        return -1;
    }

    int is_strict     = strcmp(sc->profile, "strict")     == 0;
    int is_permissive = strcmp(sc->profile, "permissive") == 0;
    int is_custom     = strcmp(sc->profile, "custom")     == 0;

    if (!is_strict && !is_permissive && !is_custom) {
        fprintf(stderr,
                "sandbox: unknown profile \"%s\" (expected strict, "
                "permissive, or custom)\n", sc->profile);
        return -1;
    }

    if (is_custom) {
        if (!sc->allowed_paths || sc->allowed_paths[0] == '\0') {
            fprintf(stderr,
                    "sandbox: custom profile requires allowed_paths\n");
            return -1;
        }
        if (!sc->allowed_commands || sc->allowed_commands[0] == '\0') {
            fprintf(stderr,
                    "sandbox: custom profile requires allowed_commands\n");
            return -1;
        }
    }

    /* Check OS enforcement availability for strict mode. */
    if (is_strict) {
#if !defined(__APPLE__) && !defined(__linux__)
        fprintf(stderr,
                "sandbox: strict profile requires OS sandbox support "
                "(macOS or Linux), but neither backend is compiled in\n");
        return -1;
#endif
    }

    return 0;
}

/* =========================================================================
 * macOS backend
 * ====================================================================== */

#if defined(__APPLE__) || defined(SANDBOX_TEST)

/* Append a string literal to tmp[]; advance pos; jump to truncated if full. */
static int sbpl_append_str(char *tmp, int pos, int cap, const char *s)
{
    int len = (int)strlen(s);
    if (len > cap - pos)
        return -1;
    memcpy(tmp + pos, s, (size_t)len);
    return pos + len;
}

int sandbox_sbpl_build(const SandboxConfig *sc, char *buf, size_t bufsz)
{
    if (!sc || !buf || bufsz == 0)
        return -1;

    char tmp[4096];
    int  pos = 0;
    int  cap = (int)sizeof(tmp) - 1;

#define SAPPEND(s) \
    do { \
        pos = sbpl_append_str(tmp, pos, cap, (s)); \
        if (pos < 0) { pos = cap; goto truncated; } \
    } while (0)

    SAPPEND("(version 1)\n");
    SAPPEND("(deny default)\n");
    SAPPEND("(allow process-exec*)\n");
    SAPPEND("(allow signal)\n");
    SAPPEND("(allow sysctl-read)\n");

    /* Per-path allow rules. */
    if (sc->allowed_paths && sc->allowed_paths[0] != '\0') {
        const char *p = sc->allowed_paths;
        char path[512];
        while (next_segment(&p, path, sizeof(path))) {
            SAPPEND("(allow file-read* file-write* (subpath \"");
            SAPPEND(path);
            SAPPEND("\"))\n");
        }
    }

    /* Network. */
    if (sc->network)
        SAPPEND("(allow network*)\n");

truncated:
    tmp[pos < 0 ? cap : pos] = '\0';

    /* Copy to caller's buffer. */
    size_t needed = (size_t)(pos < 0 ? cap : pos);
    size_t copy   = needed < bufsz ? needed : bufsz - 1;
    memcpy(buf, tmp, copy);
    buf[copy] = '\0';

    return (int)(needed < bufsz ? (int)needed : -1);

#undef SAPPEND
}

#endif /* __APPLE__ || SANDBOX_TEST */

/* =========================================================================
 * Linux backend
 * ====================================================================== */

#ifdef __linux__

static int sandbox_activate_linux(const SandboxConfig *sc)
{
    int is_strict = strcmp(sc->profile, "strict") == 0;

    /* Probe Landlock ABI version. */
    int abi = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
        if (errno == ENOSYS || errno == EOPNOTSUPP) {
            if (is_strict) {
                fprintf(stderr,
                        "sandbox: Landlock not supported by kernel "
                        "(requires 5.13+) — strict mode cannot continue\n");
                return -1;
            }
            fprintf(stderr,
                    "sandbox: Landlock not available, "
                    "running unconfined (permissive mode)\n");
            return 0;
        }
        if (is_strict) {
            fprintf(stderr,
                    "sandbox: landlock_create_ruleset version probe failed: "
                    "%s\n", strerror(errno));
            return -1;
        }
        fprintf(stderr,
                "sandbox: Landlock probe failed (%s), "
                "continuing unconfined\n", strerror(errno));
        return 0;
    }

    /* Create ruleset covering all basic FS access rights. */
    struct ll_ruleset_attr rattr = { .handled_access_fs = LL_FS_READ };
    int rfd = ll_create_ruleset(&rattr, sizeof(rattr), 0);
    if (rfd < 0) {
        if (is_strict) {
            fprintf(stderr,
                    "sandbox: landlock_create_ruleset failed: %s\n",
                    strerror(errno));
            return -1;
        }
        fprintf(stderr,
                "sandbox: landlock_create_ruleset failed (%s), "
                "continuing unconfined\n", strerror(errno));
        return 0;
    }

    /* Add a rule for each allowed path. */
    if (sc->allowed_paths && sc->allowed_paths[0] != '\0') {
        const char *p = sc->allowed_paths;
        char path[512];
        while (next_segment(&p, path, sizeof(path))) {
            int fd = open(path, O_PATH | O_RDONLY);
            if (fd < 0) {
                fprintf(stderr,
                        "sandbox: cannot open allowed path \"%s\": %s\n",
                        path, strerror(errno));
                if (is_strict) {
                    close(rfd);
                    return -1;
                }
                continue;
            }
            struct ll_path_beneath_attr pattr = {
                .allowed_access = LL_FS_READ,
                .parent_fd      = fd
            };
            int rc = ll_add_rule(rfd, LANDLOCK_RULE_PATH_BENEATH,
                                 &pattr, 0);
            close(fd);
            if (rc < 0) {
                fprintf(stderr,
                        "sandbox: landlock_add_rule failed for \"%s\": %s\n",
                        path, strerror(errno));
                if (is_strict) {
                    close(rfd);
                    return -1;
                }
            }
        }
    }

    /* Prevent privilege escalation before restrict_self. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fprintf(stderr,
                "sandbox: prctl(PR_SET_NO_NEW_PRIVS) failed: %s\n",
                strerror(errno));
        close(rfd);
        if (is_strict)
            return -1;
        return 0;
    }

    /* Activate the ruleset. */
    if (ll_restrict_self(rfd, 0) != 0) {
        close(rfd);
        if (is_strict) {
            fprintf(stderr,
                    "sandbox: landlock_restrict_self failed: %s\n",
                    strerror(errno));
            return -1;
        }
        fprintf(stderr,
                "sandbox: landlock_restrict_self failed (%s), "
                "continuing unconfined\n", strerror(errno));
        return 0;
    }

    close(rfd);
    return 0;
}

#endif /* __linux__ */

/* =========================================================================
 * Public: sandbox_activate
 * ====================================================================== */

int sandbox_activate(const SandboxConfig *sc)
{
    if (!sc)
        return -1;

    /* Disabled — nothing to do. */
    if (!sc->enabled)
        return 0;

#ifdef __APPLE__
    {
        char   sbpl[4096];
        char  *errstr = NULL;

        sandbox_sbpl_build(sc, sbpl, sizeof(sbpl));

        if (sandbox_init(sbpl, 0, &errstr) != 0) {
            fprintf(stderr, "sandbox: sandbox_init failed: %s\n",
                    errstr ? errstr : "(unknown)");
            if (errstr)
                sandbox_free_error(errstr);

            if (strcmp(sc->profile, "strict") == 0)
                return -1;
            /* permissive/custom: warn and continue */
            return 0;
        }
        return 0;
    }
#elif defined(__linux__)
    return sandbox_activate_linux(sc);
#else
    /* Unsupported OS. */
    if (strcmp(sc->profile, "strict") == 0) {
        fprintf(stderr,
                "sandbox: strict mode is not supported on this OS\n");
        return -1;
    }
    fprintf(stderr,
            "sandbox: OS enforcement not available, "
            "running unconfined (permissive mode)\n");
    return 0;
#endif
}

/* =========================================================================
 * Public: sandbox_build_prompt_block
 * ====================================================================== */

void sandbox_build_prompt_block(const SandboxConfig *sc,
                                char *buf, size_t bufsz)
{
    if (!sc || !buf || bufsz == 0)
        return;
    buf[0] = '\0';

    if (!sc->enabled)
        return;

    snprintf(buf, bufsz,
             "<sandbox_policy>\n"
             "  profile: %s\n"
             "  network: %s\n"
             "  allowed_paths: %s\n"
             "  allowed_commands: %s\n"
             "  max_file_size: %ld\n"
             "</sandbox_policy>",
             sc->profile  ? sc->profile  : "unknown",
             sc->network  ? "allowed" : "denied",
             (sc->allowed_paths    && sc->allowed_paths[0])    ? sc->allowed_paths    : "(none)",
             (sc->allowed_commands && sc->allowed_commands[0]) ? sc->allowed_commands : "(none)",
             sc->max_file_size);
}
