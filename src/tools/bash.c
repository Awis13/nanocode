/*
 * bash.c — bash tool: fork/exec, combined pipe capture, OS-native sandboxing
 *
 * Platform sandboxing applied in the child before exec:
 *   macOS : Apple Seatbelt via sandbox_init(3) — denies network and file
 *           writes outside /dev, /tmp, /private/tmp, and the working dir.
 *   Linux : seccomp-BPF (blocks network syscalls) + Landlock LSM (restricts
 *           file writes to /dev, /tmp, and the working dir), when available.
 *   Other : no sandbox — command runs with inherited permissions.
 *
 * Sandbox failure is non-fatal: a diagnostic is emitted to stderr and the
 * command runs anyway.  Callers that require hard isolation should run the
 * whole agent in a container or VM.
 *
 * Timeout is implemented with alarm(2)/SIGALRM in the parent.  The child is
 * placed in its own process group so kill(-pid, SIGKILL) terminates the entire
 * child subtree.  This is safe in nanocode's single-threaded event loop.
 */

#include "bash.h"
#include "executor.h"
#include "../../include/audit.h"
#include "../util/arena.h"
#include "../util/json.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Platform-specific sandbox headers
 * ---------------------------------------------------------------------- */

#ifdef __APPLE__
#  include <stdint.h>
/*
 * sandbox.h is deprecated but functional.  Forward-declare to avoid pulling
 * in the private header; the symbols are in libSystem on all macOS versions
 * we support.
 */
extern int  sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
extern void sandbox_free_error(char *errorbuf);
#endif /* __APPLE__ */

#ifdef __linux__
#  include <fcntl.h>
#  include <sys/prctl.h>
#  include <sys/syscall.h>
/* seccomp-BPF — available on any kernel that has CONFIG_SECCOMP_FILTER. */
#  if defined(PR_SET_SECCOMP)
#    include <linux/filter.h>
#    include <linux/seccomp.h>
#    define HAVE_SECCOMP 1
#  endif
/* Landlock LSM — kernel >= 5.13 with CONFIG_SECURITY_LANDLOCK. */
#  ifdef __has_include
#    if __has_include(<linux/landlock.h>)
#      include <linux/landlock.h>
#      define HAVE_LANDLOCK 1
#    endif
#  endif
#endif /* __linux__ */

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define MAX_CMD_LEN     4096
#define MAX_CWD_LEN     1024
#define OUTPUT_MAX      BASH_OUTPUT_MAX
#define DEFAULT_TIMEOUT BASH_DEFAULT_TIMEOUT_SECS

/* -------------------------------------------------------------------------
 * JsonCtx primitive-aware value extractor
 *
 * json_get_str() only matches JSMN_STRING tokens.  Tool args from LLMs may
 * pass numbers as JSON numbers (JSMN_PRIMITIVE), so we need to handle both.
 *
 * JsonCtx stores jsmntok_t[] type-punned into _tok[].
 * json.h guarantees sizeof(jsmntok_t) == 16 (4 × int).
 * jsmn.h defines: JSMN_OBJECT=1, JSMN_ARRAY=2, JSMN_STRING=4, JSMN_PRIMITIVE=8
 * ---------------------------------------------------------------------- */

typedef struct { int type; int start; int end; int size; } BTok;
#define BTOK_OBJECT    1   /* JSMN_OBJECT    */
#define BTOK_STRING    4   /* JSMN_STRING    */
#define BTOK_PRIMITIVE 8   /* JSMN_PRIMITIVE */

/*
 * Find `key` in the top-level JSON object and copy its string or primitive
 * value into `buf`.  Returns 0 on success, -1 if not found or buf too small.
 */
static int ctx_get_val(const JsonCtx *ctx, const char *json,
                       const char *key, char *buf, size_t cap)
{
    const BTok *t    = (const BTok *)(const void *)ctx->_tok;
    int         ntok = ctx->ntok;
    size_t      klen = strlen(key);

    if (ntok < 1 || t[0].type != BTOK_OBJECT)
        return -1;

    for (int i = 1; i < ntok - 1; i++) {
        if (t[i].type != BTOK_STRING)
            continue;
        int tlen = t[i].end - t[i].start;
        if ((int)klen != tlen)
            continue;
        if (memcmp(json + t[i].start, key, klen) != 0)
            continue;
        /* Key matched — copy the value token (string or primitive). */
        int j = i + 1;
        if (j >= ntok)
            return -1;
        if (t[j].type != BTOK_STRING && t[j].type != BTOK_PRIMITIVE)
            return -1;
        int vlen = t[j].end - t[j].start;
        if ((size_t)(vlen + 1) > cap)
            return -1;
        memcpy(buf, json + t[j].start, (size_t)vlen);
        buf[vlen] = '\0';
        return 0;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Error helper — arena-allocates a ToolResult with error=1
 * ---------------------------------------------------------------------- */

static ToolResult make_error(Arena *arena, const char *msg)
{
    size_t len  = strlen(msg);
    char  *copy = arena_alloc(arena, len + 1);
    memcpy(copy, msg, len + 1);
    ToolResult r;
    r.error   = 1;
    r.content = copy;
    r.len     = len;
    return r;
}

/* -------------------------------------------------------------------------
 * macOS Seatbelt sandbox
 * ---------------------------------------------------------------------- */

#ifdef __APPLE__
/*
 * SBPL profile: deny all by default, then explicitly allow what is needed.
 *
 * file-write-data allows writes to already-open file descriptors (pipes, ttys)
 * without requiring a path match — essential so the child can write to the
 * pipe back to the parent.
 *
 * The %s placeholder is replaced with the effective working directory.
 */
static const char s_sandbox_tmpl[] =
    "(version 1)"
    "(deny default)"
    "(allow process*)"
    "(allow signal)"
    "(allow sysctl*)"
    "(allow mach*)"
    "(allow ipc*)"
    "(allow file-read*)"
    "(allow file-write-data)"
    "(allow file-write* (subpath \"/dev\"))"
    "(allow file-write* (subpath \"/tmp\"))"
    "(allow file-write* (subpath \"/private/tmp\"))"
    "(allow file-write* (subpath \"%s\"))";

static void apply_macos_sandbox(const char *cwd)
{
    char profile[sizeof(s_sandbox_tmpl) + MAX_CWD_LEN + 4];
    int  n = snprintf(profile, sizeof(profile), s_sandbox_tmpl,
                      (cwd && cwd[0]) ? cwd : "/nonexistent");
    if (n < 0 || (size_t)n >= sizeof(profile))
        return; /* profile too long — skip */

    char *errbuf = NULL;
    int   rc     = sandbox_init(profile, 0, &errbuf);
    if (rc != 0) {
        fprintf(stderr, "[bash] sandbox_init: %s\n",
                errbuf ? errbuf : "(unknown error)");
        if (errbuf)
            sandbox_free_error(errbuf);
    }
}
#endif /* __APPLE__ */

/* -------------------------------------------------------------------------
 * Linux: seccomp-BPF network filter
 * ---------------------------------------------------------------------- */

#if defined(__linux__) && defined(HAVE_SECCOMP)
static void apply_seccomp_filter(void)
{
    /*
     * BPF program: load the syscall number and return EPERM for any syscall
     * that creates or uses a network socket; allow everything else.
     */
    struct sock_filter filter[] = {
        /* Load syscall number. */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (unsigned int)__builtin_offsetof(struct seccomp_data, nr)),

#define DENY_NR(nr)  \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)(nr), 0, 1), \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (unsigned int)EPERM)

        DENY_NR(__NR_socket),
        DENY_NR(__NR_connect),
        DENY_NR(__NR_bind),
        DENY_NR(__NR_listen),
        DENY_NR(__NR_accept),
        DENY_NR(__NR_accept4),
        DENY_NR(__NR_sendto),
        DENY_NR(__NR_sendmsg),
        DENY_NR(__NR_sendmmsg),
        DENY_NR(__NR_recvfrom),
        DENY_NR(__NR_recvmsg),
        DENY_NR(__NR_recvmmsg),

#undef DENY_NR

        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };

    struct sock_fprog prog = {
        .len    = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    /* PR_SET_NO_NEW_PRIVS is required before installing a seccomp filter. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        return;

    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}
#endif /* __linux__ && HAVE_SECCOMP */

/* -------------------------------------------------------------------------
 * Linux: Landlock filesystem restrictions
 * ---------------------------------------------------------------------- */

#if defined(__linux__) && defined(HAVE_LANDLOCK)

/*
 * Syscall numbers — glibc may not expose these on older toolchains.
 * The numbers are stable for x86-64; other arches need their own values but
 * are conditionally compiled out if <linux/landlock.h> is absent.
 */
#ifndef SYS_landlock_create_ruleset
#  define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#  define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#  define SYS_landlock_restrict_self 446
#endif

static int ll_create(const struct landlock_ruleset_attr *a, size_t sz, uint32_t f)
{
    return (int)syscall(SYS_landlock_create_ruleset, a, sz, f);
}
static int ll_add_rule(int fd, enum landlock_rule_type t, const void *a, uint32_t f)
{
    return (int)syscall(SYS_landlock_add_rule, fd, t, a, f);
}
static int ll_restrict(int fd, uint32_t f)
{
    return (int)syscall(SYS_landlock_restrict_self, fd, f);
}

/*
 * Apply Landlock: allow reads/exec everywhere; allow writes only in
 * /dev, /tmp, and the working directory.
 */
static void apply_landlock(const char *cwd)
{
    /* All write-related access rights we want to control. */
    const __u64 write_rights =
        LANDLOCK_ACCESS_FS_WRITE_FILE  |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR  |
        LANDLOCK_ACCESS_FS_MAKE_REG    |
        LANDLOCK_ACCESS_FS_MAKE_DIR    |
        LANDLOCK_ACCESS_FS_MAKE_SYM    |
        LANDLOCK_ACCESS_FS_MAKE_SOCK   |
        LANDLOCK_ACCESS_FS_MAKE_FIFO   |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK  |
        LANDLOCK_ACCESS_FS_MAKE_CHAR;

    struct landlock_ruleset_attr ra = { .handled_access_fs = write_rights };
    int ruleset_fd = ll_create(&ra, sizeof(ra), 0);
    if (ruleset_fd < 0)
        return; /* kernel too old or landlock not enabled */

    /* Grant write access to the allowed directories. */
    const char *allowed[] = { "/dev", "/tmp", cwd, NULL };
    for (int i = 0; allowed[i]; i++) {
        if (!allowed[i][0])
            continue;
        int dir_fd = open(allowed[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd < 0)
            continue;
        struct landlock_path_beneath_attr pa = {
            .allowed_access = write_rights,
            .parent_fd      = dir_fd,
        };
        ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pa, 0);
        close(dir_fd);
    }

    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    ll_restrict(ruleset_fd, 0);
    close(ruleset_fd);
}
#endif /* __linux__ && HAVE_LANDLOCK */

/* -------------------------------------------------------------------------
 * apply_sandbox — dispatch to the platform implementation
 * ---------------------------------------------------------------------- */

static void apply_sandbox(const char *cwd)
{
#if defined(__APPLE__)
    apply_macos_sandbox(cwd);
#elif defined(__linux__)
#  if defined(HAVE_LANDLOCK)
    apply_landlock(cwd);
#  endif
#  if defined(HAVE_SECCOMP)
    apply_seccomp_filter();
#  endif
    (void)cwd;
#else
    (void)cwd;
#endif
}

/* -------------------------------------------------------------------------
 * Command allow/deny filter state
 * ---------------------------------------------------------------------- */

static char        s_allowed_cmds[1024];
static char        s_denied_cmds[1024];
static AuditLog   *s_bash_audit_log     = NULL;
static const char *s_bash_audit_session = NULL;
static const char *s_bash_audit_sandbox = NULL;

void bash_set_audit(AuditLog *log, const char *session_id,
                    const char *sandbox_profile)
{
    s_bash_audit_log     = log;
    s_bash_audit_session = session_id;
    s_bash_audit_sandbox = sandbox_profile;
}

void bash_set_cmd_filter(const char *allowed_colon_sep,
                         const char *denied_colon_sep)
{
    if (allowed_colon_sep) {
        strncpy(s_allowed_cmds, allowed_colon_sep, sizeof(s_allowed_cmds) - 1);
        s_allowed_cmds[sizeof(s_allowed_cmds) - 1] = '\0';
    } else {
        s_allowed_cmds[0] = '\0';
    }
    if (denied_colon_sep) {
        strncpy(s_denied_cmds, denied_colon_sep, sizeof(s_denied_cmds) - 1);
        s_denied_cmds[sizeof(s_denied_cmds) - 1] = '\0';
    } else {
        s_denied_cmds[0] = '\0';
    }
}

/*
 * Check whether `name` appears in a colon-separated list.
 * Returns 1 if found, 0 if not.
 */
static int cmd_in_list(const char *list, const char *name)
{
    char copy[1024];
    strncpy(copy, list, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *p = copy;
    while (p) {
        char *colon = strchr(p, ':');
        if (colon)
            *colon = '\0';
        if (*p && strcmp(p, name) == 0)
            return 1;
        p = colon ? colon + 1 : NULL;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * SIGALRM handler — kills the child process group on timeout
 * ---------------------------------------------------------------------- */

static volatile pid_t g_child_pid = (pid_t)-1;

static void sigalrm_handler(int sig)
{
    (void)sig;
    pid_t p = g_child_pid;
    if (p > (pid_t)0)
        kill(-p, SIGKILL); /* SIGKILL the entire process group */
}

/* -------------------------------------------------------------------------
 * bash_handler — ToolHandler registered with the executor
 *
 * Expected args_json fields:
 *   "command"  (string,  required) : shell command to execute
 *   "timeout"  (integer, optional) : timeout in seconds (default: 30)
 *   "cwd"      (string,  optional) : working directory (default: inherited)
 * ---------------------------------------------------------------------- */

static ToolResult bash_handler(Arena *arena, const char *args_json)
{
    char cmd[MAX_CMD_LEN] = {0};
    char cwd[MAX_CWD_LEN] = {0};
    char tmout_buf[16]    = {0};
    int  timeout_secs     = DEFAULT_TIMEOUT;

    /* Parse args. */
    JsonCtx ctx;
    size_t  jlen = strlen(args_json);
    if (json_parse_ctx(&ctx, args_json, jlen) <= 0)
        return make_error(arena, "bash: invalid args JSON");

    if (ctx_get_val(&ctx, args_json, "command", cmd, sizeof(cmd)) != 0)
        return make_error(arena, "bash: missing required argument: command");

    ctx_get_val(&ctx, args_json, "cwd", cwd, sizeof(cwd));

    if (ctx_get_val(&ctx, args_json, "timeout", tmout_buf, sizeof(tmout_buf)) == 0) {
        int t = atoi(tmout_buf);
        if (t > 0)
            timeout_secs = t;
    }

    /* Apply command allow/deny filter before allocating any resources. */
    {
        /* Extract first whitespace-delimited token of cmd. */
        char cmd_base[256];
        size_t tlen = strcspn(cmd, " \t\r\n");
        if (tlen >= sizeof(cmd_base))
            tlen = sizeof(cmd_base) - 1;
        memcpy(cmd_base, cmd, tlen);
        cmd_base[tlen] = '\0';

        /* Strip directory prefix to get bare basename. */
        char *slash = strrchr(cmd_base, '/');
        if (slash)
            memmove(cmd_base, slash + 1, strlen(slash)); /* includes NUL */

        /* Denylist check first. */
        if (s_denied_cmds[0] && cmd_in_list(s_denied_cmds, cmd_base)) {
            audit_sandbox_deny(s_bash_audit_log, cmd_base,
                               s_bash_audit_session, s_bash_audit_sandbox);
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "{\"error\":\"command denied\",\"cmd\":\"%s\"}", cmd_base);
            return make_error(arena, msg);
        }

        /* Allowlist check second. */
        if (s_allowed_cmds[0] && !cmd_in_list(s_allowed_cmds, cmd_base)) {
            audit_sandbox_deny(s_bash_audit_log, cmd_base,
                               s_bash_audit_session, s_bash_audit_sandbox);
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "{\"error\":\"command not in allowlist\",\"cmd\":\"%s\"}",
                     cmd_base);
            return make_error(arena, msg);
        }
    }

    /* Create pipe: child writes stdout+stderr, parent reads. */
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return make_error(arena, "bash: pipe() failed");

    /* Allocate the output buffer from the arena up front. */
    char  *outbuf = arena_alloc(arena, OUTPUT_MAX + 1);
    size_t outlen = 0;

    /* Install SIGALRM handler, saving the previous one. */
    struct sigaction sa_new, sa_old;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = sigalrm_handler;
    sigemptyset(&sa_new.sa_mask);
    sigaction(SIGALRM, &sa_new, &sa_old);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        sigaction(SIGALRM, &sa_old, NULL);
        return make_error(arena, "bash: fork() failed");
    }

    /* =================================================================== */
    /* CHILD                                                                */
    /* =================================================================== */
    if (pid == 0) {
        /*
         * New process group — allows the parent to kill(-pid, SIGKILL) and
         * take down any subprocesses the command spawned.
         */
        setpgid(0, 0);

        /* Redirect stdout and stderr to the pipe write end. */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* Change directory if requested. */
        if (cwd[0] != '\0') {
            if (chdir(cwd) != 0) {
                fprintf(stderr, "bash: chdir(%s): %s\n", cwd, strerror(errno));
                _exit(1);
            }
        }

        /* Apply OS-native sandbox before exec. */
        apply_sandbox(cwd[0] ? cwd : ".");

        /* Replace process image with /bin/sh. */
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);

        /* execl failed — should never reach here. */
        fprintf(stderr, "bash: execl: %s\n", strerror(errno));
        _exit(127);
    }

    /* =================================================================== */
    /* PARENT                                                               */
    /* =================================================================== */

    close(pipefd[1]); /* close write end — we only read */

    g_child_pid = pid;
    alarm((unsigned int)timeout_secs);

    /* Drain the pipe. */
    int     timed_out = 0;
    ssize_t nread;
    while (outlen < OUTPUT_MAX) {
        nread = read(pipefd[0], outbuf + outlen, OUTPUT_MAX - outlen);
        if (nread > 0) {
            outlen += (size_t)nread;
        } else if (nread == 0) {
            break; /* EOF: child closed its write end */
        } else {
            if (errno == EINTR) {
                timed_out = 1; /* woken by SIGALRM */
            }
            break;
        }
    }

    /* Cancel pending alarm and restore previous handler. */
    alarm(0);
    g_child_pid = (pid_t)-1;
    sigaction(SIGALRM, &sa_old, NULL);

    close(pipefd[0]);

    /* Reap child. */
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    outbuf[outlen] = '\0';

    /* Determine exit code. */
    int exit_code = -1;
    if (WIFEXITED(wstatus))
        exit_code = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus))
        exit_code = -(int)WTERMSIG(wstatus);

    int error = (exit_code != 0) ? 1 : 0;

    /* Append markers for special conditions. */
    if (timed_out) {
        const char *notice = "\n[timed out]";
        size_t      nlen   = strlen(notice);
        if (outlen + nlen < OUTPUT_MAX) {
            memcpy(outbuf + outlen, notice, nlen);
            outlen += nlen;
            outbuf[outlen] = '\0';
        }
        error = 1;
    }

    if (outlen >= OUTPUT_MAX) {
        /* Overwrite the last few bytes with a truncation marker. */
        const char *trunc = "[...truncated]";
        size_t      tlen  = strlen(trunc);
        if (tlen <= OUTPUT_MAX) {
            memcpy(outbuf + OUTPUT_MAX - tlen, trunc, tlen);
            outbuf[OUTPUT_MAX] = '\0';
            outlen = OUTPUT_MAX;
        }
    }

    ToolResult r;
    r.error   = error;
    r.content = outbuf;
    r.len     = outlen;
    return r;
}

/* -------------------------------------------------------------------------
 * Tool schema (JSON function-calling spec for provider API)
 * ---------------------------------------------------------------------- */

static const char s_schema[] =
    "{"
      "\"name\":\"bash\","
      "\"description\":"
        "\"Execute a shell command in a sandboxed environment. "
        "stdout and stderr are combined. "
        "Returns captured output; sets error on non-zero exit code.\","
      "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"command\":{"
            "\"type\":\"string\","
            "\"description\":\"Shell command to run via /bin/sh -c\""
          "},"
          "\"timeout\":{"
            "\"type\":\"integer\","
            "\"description\":\"Timeout in seconds (default 30). "
            "Process is killed after this many seconds.\""
          "},"
          "\"cwd\":{"
            "\"type\":\"string\","
            "\"description\":\"Working directory. Defaults to the inherited cwd.\""
          "}"
        "},"
        "\"required\":[\"command\"]"
      "}"
    "}";

/* -------------------------------------------------------------------------
 * Public registration entry point
 * ---------------------------------------------------------------------- */

void bash_tool_register(void)
{
    tool_register("bash", s_schema, bash_handler, TOOL_SAFE_MUTATING);
}
