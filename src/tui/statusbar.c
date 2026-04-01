/*
 * statusbar.c — one-line status bar at the bottom of the terminal
 */

#include "statusbar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Cost table — USD per million tokens (input / output)
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *substr;        /* matched against model string */
    double      input_per_mtok;
    double      output_per_mtok;
} CostEntry;

static const CostEntry COST_TABLE[] = {
    { "opus",    15.0,  75.0  },
    { "sonnet",   3.0,  15.0  },
    { "haiku",    0.25,  1.25 },
    { NULL,       0.0,   0.0  }
};

/* -------------------------------------------------------------------------
 * Struct
 * ---------------------------------------------------------------------- */

struct StatusBar {
    int    fd;
    int    max_turns;
    char   model[64];
    double input_rate;   /* USD per MTok */
    double output_rate;  /* USD per MTok */
};

/* -------------------------------------------------------------------------
 * Static helpers
 * ---------------------------------------------------------------------- */

static void get_cost_rates(ProviderType type, const char *model,
                           double *in_rate, double *out_rate)
{
    *in_rate  = 0.0;
    *out_rate = 0.0;

    if (type == PROVIDER_OLLAMA || !model)
        return;

    for (int i = 0; COST_TABLE[i].substr; i++) {
        if (strstr(model, COST_TABLE[i].substr)) {
            *in_rate  = COST_TABLE[i].input_per_mtok;
            *out_rate = COST_TABLE[i].output_per_mtok;
            return;
        }
    }
}

static int get_terminal_rows(int fd)
{
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
}

/*
 * Format `n` with comma thousands separators into `buf`.
 * Handles values up to 9,999,999,999.
 */
static void fmt_thousands(long long n, char *buf, int cap)
{
    if (n < 0) n = 0;

    if (n >= 1000000000LL) {
        snprintf(buf, (size_t)cap, "%lld,%03lld,%03lld,%03lld",
                 n / 1000000000LL,
                 (n % 1000000000LL) / 1000000LL,
                 (n % 1000000LL)    / 1000LL,
                 n % 1000LL);
    } else if (n >= 1000000LL) {
        snprintf(buf, (size_t)cap, "%lld,%03lld,%03lld",
                 n / 1000000LL,
                 (n % 1000000LL) / 1000LL,
                 n % 1000LL);
    } else if (n >= 1000LL) {
        snprintf(buf, (size_t)cap, "%lld,%03lld",
                 n / 1000LL,
                 n % 1000LL);
    } else {
        snprintf(buf, (size_t)cap, "%lld", n);
    }
}

/*
 * Build the visible status line (no ANSI) into `buf`.
 * Returns number of bytes written (not including NUL).
 * Exposed via statusbar_format_line for unit tests.
 */
int statusbar_format_line(const StatusBar *sb, int in_tok, int out_tok,
                          int turn, char *buf, int cap)
{
    if (!sb || cap <= 0) return 0;
    if (in_tok  < 0) in_tok  = 0;
    if (out_tok < 0) out_tok = 0;

    double cost = ((double)in_tok  / 1e6) * sb->input_rate
                + ((double)out_tok / 1e6) * sb->output_rate;

    char in_str[32], out_str[32], turn_str[32];
    fmt_thousands((long long)in_tok,  in_str,  (int)sizeof(in_str));
    fmt_thousands((long long)out_tok, out_str, (int)sizeof(out_str));

    if (sb->max_turns > 0)
        snprintf(turn_str, sizeof(turn_str), "%d/%d", turn, sb->max_turns);
    else
        /* UTF-8 ∞ (U+221E) = 0xE2 0x88 0x9E */
        snprintf(turn_str, sizeof(turn_str), "%d/\xe2\x88\x9e", turn);

    return snprintf(buf, (size_t)cap,
                    "[%s]  in: %s  out: %s  ~$%.2f  turn %s",
                    sb->model, in_str, out_str, cost, turn_str);
}

/*
 * Emit an ANSI sequence to the status bar's fd:
 *   save cursor → move to `row` → erase line → [content] → restore cursor
 * One write(2) syscall; safe to call frequently.
 */
static void emit_line(int fd, int row, const char *content)
{
    char buf[512];
    int  n;

    if (content) {
        n = snprintf(buf, sizeof(buf),
                     "\033[s"       /* save cursor */
                     "\033[%d;1H"  /* move to last row, col 1 */
                     "\033[2K"     /* erase entire line */
                     "\033[2m"     /* dim */
                     "%s"          /* visible content */
                     "\033[0m"     /* reset attributes */
                     "\033[u",     /* restore cursor */
                     row, content);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "\033[s"
                     "\033[%d;1H"
                     "\033[2K"
                     "\033[u",
                     row);
    }

    if (n > 0 && n < (int)sizeof(buf))
        (void)write(fd, buf, (size_t)n);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

StatusBar *statusbar_new(int fd, const ProviderConfig *cfg, int max_turns)
{
    StatusBar *sb = calloc(1, sizeof(*sb));
    if (!sb) return NULL;

    sb->fd        = fd;
    sb->max_turns = max_turns;

    if (cfg && cfg->model)
        snprintf(sb->model, sizeof(sb->model), "%s", cfg->model);
    else
        snprintf(sb->model, sizeof(sb->model), "unknown");

    if (cfg)
        get_cost_rates(cfg->type, cfg->model, &sb->input_rate, &sb->output_rate);

    return sb;
}

void statusbar_update(StatusBar *sb, int in_tok, int out_tok, int turn)
{
    if (!sb) return;

    char content[256];
    statusbar_format_line(sb, in_tok, out_tok, turn, content, (int)sizeof(content));
    emit_line(sb->fd, get_terminal_rows(sb->fd), content);
}

void statusbar_clear(StatusBar *sb)
{
    if (!sb) return;
    emit_line(sb->fd, get_terminal_rows(sb->fd), NULL);
}

void statusbar_free(StatusBar *sb)
{
    free(sb);
}
