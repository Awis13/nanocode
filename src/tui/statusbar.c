/*
 * statusbar.c — one-line status bar at the bottom of the terminal
 */

#include "statusbar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
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
 * Monotonic clock helper
 * ---------------------------------------------------------------------- */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* -------------------------------------------------------------------------
 * Struct
 * ---------------------------------------------------------------------- */

struct StatusBar {
    int    fd;
    int    max_turns;
    char   model[64];
    double input_rate;        /* USD per MTok */
    double output_rate;       /* USD per MTok */
    Pet   *pet;               /* optional pet — NULL if none */

    /* Timing — set via statusbar_set_session_start / statusbar_mark_* */
    long long session_start_ms;  /* monotonic ms, 0 = not set */
    long long turn_start_ms;     /* set by statusbar_mark_turn_start() */
    long long latency_ms;        /* time-to-first-token; 0 = not measured yet */
    int       latency_valid;     /* 1 once first token seen this turn */
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

static int get_terminal_cols(int fd)
{
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/*
 * Return 1 if ANSI color output is appropriate for this process.
 * Disabled when NO_COLOR is set or TERM=dumb.
 */
static int color_enabled(void)
{
    const char *no_color = getenv("NO_COLOR");
    if (no_color) return 0;
    const char *term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) return 0;
    return 1;
}

/*
 * Render the pet frame (4 lines) at the right side of the terminal,
 * rows (last_row - 4) through (last_row - 1).  The frame string is
 * '\n'-separated; each line is about 8 visible characters wide.
 *
 * Skips rendering if terminal is too small or pet is PET_OFF.
 */
static void render_pet(int fd, Pet *pet, int last_row)
{
    int cols = get_terminal_cols(fd);
    if (cols < 40 || last_row < 10) return;
    if (pet->kind == PET_OFF) return;

    const char *frame = pet_frame(pet, color_enabled());

    /* Split the '\n'-separated frame into 4 lines and emit each one. */
    char buf[1024];
    int  n     = 0;
    int  col   = cols - 8;  /* right-align: 8 visible chars wide */
    int  row   = last_row - 4;

    const char *p = frame;
    for (int line = 0; line < 4 && *p; line++) {
        /* Find end of this line. */
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        /* Build ANSI sequence: save cursor, move, erase 8 chars, write line, restore. */
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "\033[s"         /* save cursor */
                      "\033[%d;%dH"   /* move to row, col */
                      "%.*s"           /* line content (may include ANSI codes) */
                      "\033[u",        /* restore cursor */
                      row + line, col,
                      (int)len, p);

        p = end ? end + 1 : p + len;
    }

    if (n > 0 && n < (int)sizeof(buf))
        (void)write(fd, buf, (size_t)n);
}

/*
 * Clear the 4-line pet area (rows last_row-4 through last_row-1).
 */
static void clear_pet(int fd, int last_row)
{
    int cols = get_terminal_cols(fd);
    if (cols < 40 || last_row < 10) return;

    char buf[512];
    int  n   = 0;
    int  col = cols - 8;

    for (int line = 0; line < 4; line++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "\033[s"
                      "\033[%d;%dH"
                      "        "    /* 8 spaces — erase pet area */
                      "\033[u",
                      last_row - 4 + line, col);
    }

    if (n > 0 && n < (int)sizeof(buf))
        (void)write(fd, buf, (size_t)n);
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
 * Format elapsed session time into `buf` (e.g. "3m12s", "42s", "1h02m").
 * Returns bytes written (no NUL); writes empty string if elapsed <= 0.
 */
static int fmt_elapsed(long long elapsed_ms, char *buf, int cap)
{
    if (elapsed_ms <= 0 || cap <= 0) {
        if (cap > 0) buf[0] = '\0';
        return 0;
    }
    long long s = elapsed_ms / 1000LL;
    if (s < 60)
        return snprintf(buf, (size_t)cap, "%llds", s);
    if (s < 3600)
        return snprintf(buf, (size_t)cap, "%lldm%02llds", s / 60, s % 60);
    return snprintf(buf, (size_t)cap,
                    "%lldh%02lldm", s / 3600, (s % 3600) / 60);
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

    int n = snprintf(buf, (size_t)cap,
                     "[%s]  in: %s  out: %s  ~$%.2f  turn %s",
                     sb->model, in_str, out_str, cost, turn_str);

    /* Append latency (time-to-first-token) if measured. */
    if (sb->latency_valid && n > 0 && n < cap - 1) {
        int r;
        if (sb->latency_ms < 1000)
            r = snprintf(buf + n, (size_t)(cap - n),
                         "  %lldms", sb->latency_ms);
        else
            r = snprintf(buf + n, (size_t)(cap - n),
                         "  %.1fs", (double)sb->latency_ms / 1000.0);
        if (r > 0) n += r;
    }

    /* Append session elapsed time if session_start was set. */
    if (sb->session_start_ms > 0 && n > 0 && n < cap - 1) {
        long long elapsed = now_ms() - sb->session_start_ms;
        char elapsed_str[32];
        int  r = fmt_elapsed(elapsed, elapsed_str, (int)sizeof(elapsed_str));
        if (r > 0 && n + r + 2 < cap) {
            buf[n++] = ' ';
            buf[n++] = ' ';
            memcpy(buf + n, elapsed_str, (size_t)r);
            n += r;
            buf[n] = '\0';
        }
    }

    return n;
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

void statusbar_set_pet(StatusBar *sb, Pet *pet)
{
    if (!sb) return;
    sb->pet = pet;
}

void statusbar_set_session_start(StatusBar *sb)
{
    if (!sb) return;
    sb->session_start_ms = now_ms();
}

void statusbar_mark_turn_start(StatusBar *sb)
{
    if (!sb) return;
    sb->turn_start_ms   = now_ms();
    sb->latency_valid   = 0;
    sb->latency_ms      = 0;
}

void statusbar_mark_first_token(StatusBar *sb)
{
    if (!sb || sb->latency_valid) return;
    if (sb->turn_start_ms > 0) {
        sb->latency_ms = now_ms() - sb->turn_start_ms;
        if (sb->latency_ms < 0) sb->latency_ms = 0;
    }
    sb->latency_valid = 1;
}

void statusbar_update(StatusBar *sb, int in_tok, int out_tok, int turn)
{
    if (!sb) return;

    int last_row = get_terminal_rows(sb->fd);

    /* Tick and render pet before the status line. */
    if (sb->pet) {
        pet_tick(sb->pet);
        render_pet(sb->fd, sb->pet, last_row);
    }

    char content[256];
    statusbar_format_line(sb, in_tok, out_tok, turn, content, (int)sizeof(content));
    emit_line(sb->fd, last_row, content);
}

void statusbar_clear(StatusBar *sb)
{
    if (!sb) return;
    int last_row = get_terminal_rows(sb->fd);
    if (sb->pet) clear_pet(sb->fd, last_row);
    emit_line(sb->fd, last_row, NULL);
}

void statusbar_free(StatusBar *sb)
{
    free(sb);
}
