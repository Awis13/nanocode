/*
 * input.c — terminal line editor
 *
 * Provides readline-like UX in raw terminal mode:
 *   - Arrow-key navigation and history (up/down)
 *   - Persistent history: ~/.nanocode/history
 *   - Tab completion for /commands
 *   - Multi-line continuation via trailing '\'
 *   - Paste detection: burst input is inserted silently, then redrawn
 *   - Ctrl+C cancels the current line; Ctrl+D on empty line exits
 */

#define _POSIX_C_SOURCE 200809L

#include "tui/input.h"

#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

/* =========================================================================
 * History ring buffer
 * ====================================================================== */

#define HIST_CAP 1000

static char *g_hist[HIST_CAP];   /* malloc'd C strings (may be NULL) */
static int   g_hist_n     = 0;   /* entries currently stored (0..HIST_CAP) */
static int   g_hist_end   = 0;   /* ring write head (0..HIST_CAP-1) */
static int   g_hist_total = 0;   /* total entries ever added (monotonic) */

/*
 * Return history entry by recency: idx 0 = most recent, idx g_hist_n-1 = oldest.
 * Returns NULL when idx is out of range.
 */
static const char *hist_peek(int idx)
{
    if (idx < 0 || idx >= g_hist_n) return NULL;
    int ring = ((g_hist_end - 1 - idx) % HIST_CAP + HIST_CAP) % HIST_CAP;
    return g_hist[ring];
}

void input_history_add(const char *line)
{
    if (!line || *line == '\0') return;
    /* Drop consecutive duplicates. */
    if (g_hist_n > 0) {
        const char *last = hist_peek(0);
        if (last && strcmp(last, line) == 0) return;
    }
    /* Free the slot we are about to overwrite (ring full). */
    if (g_hist[g_hist_end]) {
        free(g_hist[g_hist_end]);
        g_hist[g_hist_end] = NULL;
    }
    g_hist[g_hist_end] = strdup(line);
    if (!g_hist[g_hist_end]) return; /* malloc failure: silently drop */
    g_hist_end = (g_hist_end + 1) % HIST_CAP;
    if (g_hist_n < HIST_CAP) g_hist_n++;
    g_hist_total++;
}

void input_history_save(const char *path)
{
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    /* Write oldest first so load order is preserved. */
    for (int i = g_hist_n - 1; i >= 0; i--) {
        const char *e = hist_peek(i);
        if (e) fprintf(f, "%s\n", e);
    }
    fclose(f);
}

void input_history_load(const char *path)
{
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    /* Replace in-memory history. */
    input_history_reset();
    char line[INPUT_LINE_MAX];
    while (fgets(line, (int)sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        line[len] = '\0';
        if (len > 0) input_history_add(line);
    }
    fclose(f);
}

void input_history_reset(void)
{
    for (int i = 0; i < HIST_CAP; i++) {
        if (g_hist[i]) { free(g_hist[i]); g_hist[i] = NULL; }
    }
    g_hist_n     = 0;
    g_hist_end   = 0;
    g_hist_total = 0;
}

/* =========================================================================
 * EditBuf — pure editing operations (no I/O)
 * ====================================================================== */

void edit_reset(EditBuf *e)
{
    e->len    = 0;
    e->pos    = 0;
    e->buf[0] = '\0';
}

void edit_insert(EditBuf *e, char c)
{
    if (e->len >= INPUT_LINE_MAX - 1) return;
    if (e->pos < e->len)
        memmove(e->buf + e->pos + 1, e->buf + e->pos, e->len - e->pos);
    e->buf[e->pos] = c;
    e->pos++;
    e->len++;
    e->buf[e->len] = '\0';
}

void edit_insert_str(EditBuf *e, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        edit_insert(e, s[i]);
}

void edit_backspace(EditBuf *e)
{
    if (e->pos == 0) return;
    memmove(e->buf + e->pos - 1, e->buf + e->pos, e->len - e->pos);
    e->pos--;
    e->len--;
    e->buf[e->len] = '\0';
}

void edit_delete(EditBuf *e)
{
    if (e->pos >= e->len) return;
    memmove(e->buf + e->pos, e->buf + e->pos + 1, e->len - e->pos - 1);
    e->len--;
    e->buf[e->len] = '\0';
}

void edit_move_left(EditBuf *e)  { if (e->pos > 0)      e->pos--; }
void edit_move_right(EditBuf *e) { if (e->pos < e->len) e->pos++; }
void edit_move_home(EditBuf *e)  { e->pos = 0; }
void edit_move_end(EditBuf *e)   { e->pos = e->len; }

/* =========================================================================
 * Tab completion
 * ====================================================================== */

static const char *const s_commands[] = {
    "/apply", "/clear", "/diff", "/help", "/mcp", "/memory", "/model",
    "/plan", "/save", NULL
};

void input_tab_complete(EditBuf *e)
{
    if (e->len == 0 || e->buf[0] != '/') return;
    /* Only complete before any space. */
    for (size_t i = 0; i < e->len; i++) {
        if (e->buf[i] == ' ') return;
    }
    e->buf[e->len] = '\0'; /* ensure NUL for strncmp */

    const char *matches[16];
    int nm = 0;
    for (int i = 0; s_commands[i] && nm < 15; i++) {
        if (strncmp(s_commands[i], e->buf, e->len) == 0)
            matches[nm++] = s_commands[i];
    }
    if (nm == 0) return;

    /* Find longest common prefix of all matches. */
    size_t prefix = e->len;
    for (;;) {
        char ch = matches[0][prefix];
        if (!ch) break;
        int ok = 1;
        for (int i = 1; i < nm; i++) {
            if (matches[i][prefix] != ch) { ok = 0; break; }
        }
        if (!ok) break;
        prefix++;
    }

    if (prefix > e->len && prefix < INPUT_LINE_MAX - 1) {
        memcpy(e->buf, matches[0], prefix);
        e->len = prefix;
        e->pos = prefix;
        e->buf[e->len] = '\0';
    }
}

/* =========================================================================
 * Raw terminal mode
 * ====================================================================== */

static struct termios g_orig_termios;
static int            g_raw_fd       = -1;
static int            g_atexit_done  = 0;

static void raw_restore(void)
{
    if (g_raw_fd >= 0) {
        tcsetattr(g_raw_fd, TCSAFLUSH, &g_orig_termios);
        g_raw_fd = -1;
    }
}

static int raw_enable(int fd)
{
    if (tcgetattr(fd, &g_orig_termios) < 0) return -1;
    g_raw_fd = fd;
    if (!g_atexit_done) {
        atexit(raw_restore);
        g_atexit_done = 1;
    }
    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |=  (unsigned)(CS8);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSAFLUSH, &raw);
}

/* =========================================================================
 * Key reading
 * ====================================================================== */

typedef enum {
    K_NONE = 0,
    K_CHAR,
    K_ENTER,
    K_BACKSPACE,
    K_DELETE,
    K_LEFT, K_RIGHT, K_HOME, K_END,
    K_UP, K_DOWN,
    K_TAB,
    K_CTRL_C,
    K_CTRL_D,
    K_CTRL_K,
    K_CTRL_L,
    K_CTRL_U,
    K_CTRL_W,
} KeyCode;

typedef struct { KeyCode code; char ch; } Key;

/* Returns 1 if data is available with zero-timeout, 0 otherwise. */
static int data_avail(int fd)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {0, 0};
    return select(fd + 1, &fds, NULL, NULL, &tv) > 0;
}

/*
 * Read one byte from fd.
 * timeout_ms == 0  → block until data arrives.
 * timeout_ms  > 0  → return 0 if no data within that window.
 */
static int read_byte(int fd, unsigned char *c, int timeout_ms)
{
    if (timeout_ms <= 0) {
        return (read(fd, c, 1) == 1) ? 1 : 0;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) return 0;
    return (read(fd, c, 1) == 1) ? 1 : 0;
}

static Key parse_key(int fd, unsigned char c)
{
    Key k = {K_NONE, 0};
    switch (c) {
    case  1: k.code = K_HOME;      break; /* Ctrl+A */
    case  3: k.code = K_CTRL_C;    break; /* Ctrl+C */
    case  4: k.code = K_CTRL_D;    break; /* Ctrl+D */
    case  5: k.code = K_END;       break; /* Ctrl+E */
    case  8: k.code = K_BACKSPACE; break; /* Ctrl+H */
    case  9: k.code = K_TAB;       break; /* Tab */
    case 10: /* fall-through */
    case 13: k.code = K_ENTER;     break; /* LF / CR */
    case 11: k.code = K_CTRL_K;    break; /* Ctrl+K */
    case 12: k.code = K_CTRL_L;    break; /* Ctrl+L */
    case 21: k.code = K_CTRL_U;    break; /* Ctrl+U */
    case 23: k.code = K_CTRL_W;    break; /* Ctrl+W */
    case 127: k.code = K_BACKSPACE; break; /* DEL */

    case 27: { /* ESC — start of ANSI escape sequence */
        unsigned char c2;
        if (!read_byte(fd, &c2, 50)) break;
        if (c2 == '[') {
            unsigned char c3;
            if (!read_byte(fd, &c3, 50)) break;
            unsigned char tilde;
            switch (c3) {
            case 'A': k.code = K_UP;    break;
            case 'B': k.code = K_DOWN;  break;
            case 'C': k.code = K_RIGHT; break;
            case 'D': k.code = K_LEFT;  break;
            case 'H': k.code = K_HOME;  break;
            case 'F': k.code = K_END;   break;
            case '1': case '7':
                read_byte(fd, &tilde, 50);
                k.code = K_HOME; break;
            case '3':
                read_byte(fd, &tilde, 50);
                k.code = K_DELETE; break;
            case '4': case '8':
                read_byte(fd, &tilde, 50);
                k.code = K_END; break;
            default: break;
            }
        } else if (c2 == 'O') {
            unsigned char c3;
            if (!read_byte(fd, &c3, 50)) break;
            switch (c3) {
            case 'A': k.code = K_UP;    break;
            case 'B': k.code = K_DOWN;  break;
            case 'C': k.code = K_RIGHT; break;
            case 'D': k.code = K_LEFT;  break;
            case 'H': k.code = K_HOME;  break;
            case 'F': k.code = K_END;   break;
            default: break;
            }
        }
        break;
    }

    default:
        if (c >= 0x20 && c < 0x7f) { k.code = K_CHAR; k.ch = (char)c; }
        break;
    }
    return k;
}

/* =========================================================================
 * Display
 * ====================================================================== */

/*
 * Overwrite the current input line in place:
 *   CR → prompt → buffer contents → erase-to-EOL → reposition cursor.
 */
static void redraw_line(int fd, const char *prompt, const EditBuf *e)
{
    /* Worst case: 1 + prompt + buf + 3 + 1 + 16 */
    char out[INPUT_LINE_MAX + 512];
    size_t olen = 0;
    size_t cap  = sizeof(out);

#define OAPPEND(data, dlen) do { \
    size_t _n = (size_t)(dlen); \
    if (olen + _n < cap) { memcpy(out + olen, (data), _n); olen += _n; } \
} while (0)

    OAPPEND("\r", 1);
    size_t plen = strlen(prompt);
    OAPPEND(prompt, plen);
    OAPPEND(e->buf, e->len);
    OAPPEND("\033[K", 3); /* erase to end of line */
    OAPPEND("\r", 1);
    size_t col = plen + e->pos;
    if (col > 0) {
        char fwd[32];
        int n = snprintf(fwd, sizeof(fwd), "\033[%dC", (int)col);
        if (n > 0) OAPPEND(fwd, (size_t)n);
    }
#undef OAPPEND

    write(fd, out, olen);
}

/* =========================================================================
 * input_read — main entry point
 * ====================================================================== */

InputLine input_read(Arena *arena, const char *prompt)
{
    InputLine result = {NULL, 0, 0};
    if (!prompt) prompt = "";

    /* ------------------------------------------------------------------ */
    /* Non-interactive path                                                */
    /* ------------------------------------------------------------------ */
    if (!isatty(STDIN_FILENO)) {
        char line[INPUT_LINE_MAX];
        if (!fgets(line, (int)sizeof(line), stdin)) {
            result.is_eof = 1;
            return result;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        char *text = arena_alloc(arena, len + 1);
        if (text) {
            memcpy(text, line, len);
            text[len] = '\0';
            result.text = text;
            result.len  = len;
        }
        return result;
    }

    /* ------------------------------------------------------------------ */
    /* Interactive path                                                    */
    /* ------------------------------------------------------------------ */
    if (raw_enable(STDIN_FILENO) < 0) {
        /* Raw mode failed; fall back to line-buffered fgets. */
        printf("%s", prompt);
        fflush(stdout);
        char line[INPUT_LINE_MAX];
        if (!fgets(line, (int)sizeof(line), stdin)) {
            result.is_eof = 1;
            return result;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        char *text = arena_alloc(arena, len + 1);
        if (text) {
            memcpy(text, line, len);
            text[len] = '\0';
            result.text = text;
            result.len  = len;
        }
        return result;
    }

    /* Accumulation buffer for multi-line continuation. */
    char   accum[INPUT_LINE_MAX * 8];
    size_t accum_len  = 0;
    const char *cprompt = prompt; /* changes to "> " on continuation lines */
    int    continuing = 1;

    while (continuing) {
        EditBuf e;
        memset(&e, 0, sizeof(e));

        /* History navigation state (local to this logical line). */
        int   hist_idx  = g_hist_total; /* sentinel: not browsing */
        char  saved[INPUT_LINE_MAX];
        size_t saved_len = 0;
        saved[0] = '\0';

        write(STDOUT_FILENO, cprompt, strlen(cprompt));

        /* ---- inner key loop ---- */
        for (;;) {
            unsigned char c;
            if (!read_byte(STDIN_FILENO, &c, 0)) {
                /* stdin closed */
                raw_restore();
                result.is_eof = 1;
                return result;
            }

            /* ---- paste detection ----------------------------------------
             * If the first char is printable and more bytes are already
             * buffered, treat the burst as paste: insert all without
             * per-character echo, then redraw once.
             * ---------------------------------------------------------------- */
            if (c >= 0x20 && c < 0x7f && data_avail(STDIN_FILENO)) {
                char   paste[INPUT_LINE_MAX];
                int    plen = 0;
                paste[plen++] = (char)c;
                int hit_enter = 0;
                while (plen < (int)sizeof(paste) - 1 &&
                       data_avail(STDIN_FILENO)) {
                    unsigned char pc;
                    if (read(STDIN_FILENO, &pc, 1) != 1) break;
                    if (pc == '\r' || pc == '\n') { hit_enter = 1; break; }
                    if (pc < 0x20) break;
                    paste[plen++] = (char)pc;
                }
                edit_insert_str(&e, paste, (size_t)plen);
                redraw_line(STDOUT_FILENO, cprompt, &e);
                if (hit_enter) {
                    write(STDOUT_FILENO, "\r\n", 2);
                    goto line_done;
                }
                continue;
            }

            Key key = parse_key(STDIN_FILENO, c);

            switch (key.code) {

            case K_CHAR:
                edit_insert(&e, key.ch);
                if (e.pos == e.len) {
                    /* Cursor at end: just emit the character. */
                    char ch = key.ch;
                    write(STDOUT_FILENO, &ch, 1);
                } else {
                    redraw_line(STDOUT_FILENO, cprompt, &e);
                }
                break;

            case K_ENTER:
                write(STDOUT_FILENO, "\r\n", 2);
                goto line_done;

            case K_BACKSPACE:
                if (e.pos > 0) {
                    edit_backspace(&e);
                    redraw_line(STDOUT_FILENO, cprompt, &e);
                }
                break;

            case K_DELETE:
                if (e.pos < e.len) {
                    edit_delete(&e);
                    redraw_line(STDOUT_FILENO, cprompt, &e);
                }
                break;

            case K_LEFT:
                if (e.pos > 0) {
                    edit_move_left(&e);
                    write(STDOUT_FILENO, "\033[D", 3);
                }
                break;

            case K_RIGHT:
                if (e.pos < e.len) {
                    edit_move_right(&e);
                    write(STDOUT_FILENO, "\033[C", 3);
                }
                break;

            case K_HOME:
                edit_move_home(&e);
                redraw_line(STDOUT_FILENO, cprompt, &e);
                break;

            case K_END:
                edit_move_end(&e);
                redraw_line(STDOUT_FILENO, cprompt, &e);
                break;

            case K_UP: {
                int steps = g_hist_total - hist_idx;
                if (steps < g_hist_n) {
                    if (steps == 0) {
                        /* Save current edit before browsing. */
                        memcpy(saved, e.buf, e.len + 1);
                        saved_len = e.len;
                    }
                    hist_idx--;
                    steps++;
                    const char *hline = hist_peek(steps - 1);
                    if (hline) {
                        edit_reset(&e);
                        edit_insert_str(&e, hline, strlen(hline));
                        redraw_line(STDOUT_FILENO, cprompt, &e);
                    }
                }
                break;
            }

            case K_DOWN: {
                int steps = g_hist_total - hist_idx;
                if (steps > 0) {
                    hist_idx++;
                    steps--;
                    if (steps == 0) {
                        edit_reset(&e);
                        edit_insert_str(&e, saved, saved_len);
                    } else {
                        const char *hline = hist_peek(steps - 1);
                        if (hline) {
                            edit_reset(&e);
                            edit_insert_str(&e, hline, strlen(hline));
                        }
                    }
                    redraw_line(STDOUT_FILENO, cprompt, &e);
                }
                break;
            }

            case K_TAB:
                input_tab_complete(&e);
                redraw_line(STDOUT_FILENO, cprompt, &e);
                break;

            case K_CTRL_C:
                /* Cancel current line; reprint prompt. */
                edit_reset(&e);
                hist_idx  = g_hist_total;
                saved_len = 0;
                saved[0]  = '\0';
                write(STDOUT_FILENO, "^C\r\n", 4);
                write(STDOUT_FILENO, cprompt, strlen(cprompt));
                break;

            case K_CTRL_D:
                if (e.len == 0 && accum_len == 0) {
                    write(STDOUT_FILENO, "\r\n", 2);
                    raw_restore();
                    result.is_eof = 1;
                    return result;
                }
                /* Delete-forward when not at EOF. */
                if (e.pos < e.len) {
                    edit_delete(&e);
                    redraw_line(STDOUT_FILENO, cprompt, &e);
                }
                break;

            case K_CTRL_K:
                e.len = e.pos;
                e.buf[e.len] = '\0';
                redraw_line(STDOUT_FILENO, cprompt, &e);
                break;

            case K_CTRL_U:
                memmove(e.buf, e.buf + e.pos, e.len - e.pos);
                e.len -= e.pos;
                e.pos  = 0;
                e.buf[e.len] = '\0';
                redraw_line(STDOUT_FILENO, cprompt, &e);
                break;

            case K_CTRL_W:
                while (e.pos > 0 && e.buf[e.pos - 1] == ' ')
                    edit_backspace(&e);
                while (e.pos > 0 && e.buf[e.pos - 1] != ' ')
                    edit_backspace(&e);
                redraw_line(STDOUT_FILENO, cprompt, &e);
                break;

            case K_CTRL_L:
                write(STDOUT_FILENO, "\033[2J\033[H", 7);
                redraw_line(STDOUT_FILENO, cprompt, &e);
                break;

            case K_NONE:
            default:
                break;
            }
        } /* inner key loop */

    line_done:
        /* ---- multi-line continuation ------------------------------------ */
        if (e.len > 0 && e.buf[e.len - 1] == '\\') {
            /* Strip the backslash and append a newline to the accumulator. */
            e.len--;
            e.buf[e.len] = '\0';
            if (accum_len + e.len + 1 < sizeof(accum) - 1) {
                memcpy(accum + accum_len, e.buf, e.len);
                accum_len += e.len;
                accum[accum_len++] = '\n';
            }
            cprompt = "> ";
        } else {
            /* Final (or only) line: append and stop. */
            if (accum_len + e.len < sizeof(accum)) {
                memcpy(accum + accum_len, e.buf, e.len);
                accum_len += e.len;
            }
            continuing = 0;
        }
    } /* multi-line while */

    raw_restore();

    char *text = arena_alloc(arena, accum_len + 1);
    if (text) {
        memcpy(text, accum, accum_len);
        text[accum_len] = '\0';
        result.text = text;
        result.len  = accum_len;
    }
    return result;
}

/* =========================================================================
 * Event-driven input context (CMP-181)
 *
 * Registers STDIN_FILENO with the main event loop so that input events are
 * handled in the same loop_step() as API (streaming) events.  The blocking
 * read_byte(fd, c, 0) call that previously stalled the loop is replaced by
 * a loop_cb that drains all immediately available bytes on each readiness
 * notification.
 * ====================================================================== */

struct InputCtx {
    Arena         *arena;
    const char    *prompt;   /* original prompt string (caller-owned) */
    const char    *cprompt;  /* current prompt: prompt or "> " for continuation */

    EditBuf        e;

    char           accum[INPUT_LINE_MAX * 8]; /* multi-line accumulator */
    size_t         accum_len;

    int            hist_idx;                  /* g_hist_total = not browsing */
    char           saved[INPUT_LINE_MAX];     /* saved line during history nav */
    size_t         saved_len;

    input_line_cb  on_line;
    void          *userdata;
};

/* Reset all per-line editing state without touching on_line / arena / prompt. */
static void ctx_reset_state(InputCtx *ctx)
{
    memset(&ctx->e, 0, sizeof(ctx->e));
    ctx->accum_len  = 0;
    ctx->accum[0]   = '\0';
    ctx->cprompt    = ctx->prompt;
    ctx->hist_idx   = g_hist_total;
    ctx->saved_len  = 0;
    ctx->saved[0]   = '\0';
}

InputCtx *input_ctx_new(Arena *arena, const char *prompt,
                        input_line_cb on_line, void *userdata)
{
    InputCtx *ctx = calloc(1, sizeof(InputCtx));
    if (!ctx) return NULL;

    ctx->arena    = arena;
    ctx->prompt   = prompt ? prompt : "";
    ctx->on_line  = on_line;
    ctx->userdata = userdata;
    ctx_reset_state(ctx);

    /* Enable raw mode and display the initial prompt when connected to a tty. */
    if (isatty(STDIN_FILENO)) {
        raw_enable(STDIN_FILENO);
        write(STDOUT_FILENO, ctx->prompt, strlen(ctx->prompt));
    }
    return ctx;
}

void input_ctx_free(InputCtx *ctx)
{
    if (!ctx) return;
    raw_restore();
    free(ctx);
}

void input_ctx_reset(InputCtx *ctx, Arena *arena, const char *prompt)
{
    if (!ctx) return;
    ctx->arena  = arena;
    ctx->prompt = prompt ? prompt : "";
    ctx_reset_state(ctx);
    if (isatty(STDOUT_FILENO))
        write(STDOUT_FILENO, ctx->prompt, strlen(ctx->prompt));
}

/*
 * Append the current edit buffer to the accumulator and fire on_line.
 * Resets per-line state and re-displays the prompt for the next line.
 */
static void ctx_emit_line(InputCtx *ctx)
{
    size_t avail   = sizeof(ctx->accum) - ctx->accum_len;
    size_t to_copy = ctx->e.len < avail ? ctx->e.len : avail;
    memcpy(ctx->accum + ctx->accum_len, ctx->e.buf, to_copy);
    ctx->accum_len += to_copy;

    InputLine result;
    result.is_eof = 0;
    result.len    = ctx->accum_len;
    result.text   = NULL;
    char *text = arena_alloc(ctx->arena, ctx->accum_len + 1);
    if (text) {
        memcpy(text, ctx->accum, ctx->accum_len);
        text[ctx->accum_len] = '\0';
        result.text = text;
    }

    ctx->on_line(result, ctx->userdata);

    /* Reset for the next line. */
    ctx_reset_state(ctx);
    if (isatty(STDOUT_FILENO))
        write(STDOUT_FILENO, ctx->prompt, strlen(ctx->prompt));
}

/*
 * Process an Enter keypress: handle multi-line continuation or emit the line.
 * Writes "\r\n" before taking action.
 *
 * Returns:
 *   -1  EOF was signalled (from within ctx_emit_line, shouldn't happen here,
 *       but keeps the signature consistent with the K_CTRL_D path)
 *    0  continuation line started — caller may keep draining
 *    1  complete line emitted — caller must stop draining and return 0
 */
static int ctx_handle_enter(InputCtx *ctx)
{
    write(STDOUT_FILENO, "\r\n", 2);

    if (ctx->e.len > 0 && ctx->e.buf[ctx->e.len - 1] == '\\') {
        /* Multi-line continuation: strip trailing '\', append to accumulator. */
        ctx->e.len--;
        ctx->e.buf[ctx->e.len] = '\0';

        size_t avail   = sizeof(ctx->accum) - ctx->accum_len;
        size_t to_copy = ctx->e.len < avail ? ctx->e.len : avail;
        memcpy(ctx->accum + ctx->accum_len, ctx->e.buf, to_copy);
        ctx->accum_len += to_copy;
        if (ctx->accum_len < sizeof(ctx->accum) - 1)
            ctx->accum[ctx->accum_len++] = '\n';

        ctx->cprompt  = "> ";
        ctx->hist_idx = g_hist_total;
        memset(&ctx->e, 0, sizeof(ctx->e));
        ctx->saved_len = 0;
        ctx->saved[0]  = '\0';
        write(STDOUT_FILENO, ctx->cprompt, strlen(ctx->cprompt));
        return 0; /* continuation: keep draining */
    } else {
        ctx_emit_line(ctx);
        return 1; /* line emitted: stop draining */
    }
}

int input_on_readable(int fd, int events, void *ctx_)
{
    (void)events;
    InputCtx *ctx = (InputCtx *)ctx_;

    /*
     * Drain all immediately available bytes.  This is required for
     * edge-triggered epoll (Linux) — if bytes are left unread we won't
     * receive another readiness notification until new data arrives.
     * On macOS kqueue (level-triggered) it is an optimisation that
     * processes bursts (paste, escape sequences) in a single callback.
     *
     * The first byte is guaranteed present by the event loop.  Each
     * subsequent iteration is gated on data_avail() so we never block.
     */
    int first = 1;
    while (first || data_avail(fd)) {
        first = 0;

        unsigned char c;
        if (read(fd, &c, 1) != 1) {
            /* EOF (read returned 0) or unrecoverable error. */
            raw_restore();
            write(STDOUT_FILENO, "\r\n", 2);
            InputLine eof = {NULL, 0, 1};
            ctx->on_line(eof, ctx->userdata);
            return -1;
        }

        /* ---- paste detection ------------------------------------------- */
        if (c >= 0x20 && c < 0x7f && data_avail(fd)) {
            char paste[INPUT_LINE_MAX];
            int  plen      = 0;
            int  hit_enter = 0;
            paste[plen++]  = (char)c;
            while (plen < (int)sizeof(paste) - 1 && data_avail(fd)) {
                unsigned char pc;
                if (read(fd, &pc, 1) != 1) break;
                if (pc == '\r' || pc == '\n') { hit_enter = 1; break; }
                if (pc < 0x20) break; /* non-printable: stop, byte consumed */
                paste[plen++] = (char)pc;
            }
            edit_insert_str(&ctx->e, paste, (size_t)plen);
            redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            if (hit_enter) {
                int er = ctx_handle_enter(ctx);
                if (er < 0) return -1;
                if (er > 0) return 0; /* line emitted: stop draining */
            }
            continue;
        }

        /* ---- normal key ------------------------------------------------- */
        Key key = parse_key(fd, c); /* may do short blocking reads for ESC */

        switch (key.code) {

        case K_CHAR:
            edit_insert(&ctx->e, key.ch);
            if (ctx->e.pos == ctx->e.len) {
                char ch = key.ch;
                write(STDOUT_FILENO, &ch, 1);
            } else {
                redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            }
            break;

        case K_ENTER: {
            int er = ctx_handle_enter(ctx);
            if (er < 0) return -1;
            if (er > 0) return 0; /* line emitted: stop draining */
            break;
        }

        case K_BACKSPACE:
            if (ctx->e.pos > 0) {
                edit_backspace(&ctx->e);
                redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            }
            break;

        case K_DELETE:
            if (ctx->e.pos < ctx->e.len) {
                edit_delete(&ctx->e);
                redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            }
            break;

        case K_LEFT:
            if (ctx->e.pos > 0) {
                edit_move_left(&ctx->e);
                write(STDOUT_FILENO, "\033[D", 3);
            }
            break;

        case K_RIGHT:
            if (ctx->e.pos < ctx->e.len) {
                edit_move_right(&ctx->e);
                write(STDOUT_FILENO, "\033[C", 3);
            }
            break;

        case K_HOME:
            edit_move_home(&ctx->e);
            redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            break;

        case K_END:
            edit_move_end(&ctx->e);
            redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            break;

        case K_UP: {
            int steps = g_hist_total - ctx->hist_idx;
            if (steps < g_hist_n) {
                if (steps == 0) {
                    memcpy(ctx->saved, ctx->e.buf, ctx->e.len + 1);
                    ctx->saved_len = ctx->e.len;
                }
                ctx->hist_idx--;
                steps++;
                const char *hline = hist_peek(steps - 1);
                if (hline) {
                    edit_reset(&ctx->e);
                    edit_insert_str(&ctx->e, hline, strlen(hline));
                    redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
                }
            }
            break;
        }

        case K_DOWN: {
            int steps = g_hist_total - ctx->hist_idx;
            if (steps > 0) {
                ctx->hist_idx++;
                steps--;
                if (steps == 0) {
                    edit_reset(&ctx->e);
                    edit_insert_str(&ctx->e, ctx->saved, ctx->saved_len);
                } else {
                    const char *hline = hist_peek(steps - 1);
                    if (hline) {
                        edit_reset(&ctx->e);
                        edit_insert_str(&ctx->e, hline, strlen(hline));
                    }
                }
                redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            }
            break;
        }

        case K_TAB:
            input_tab_complete(&ctx->e);
            redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            break;

        case K_CTRL_C:
            memset(&ctx->e, 0, sizeof(ctx->e));
            ctx->hist_idx  = g_hist_total;
            ctx->saved_len = 0;
            ctx->saved[0]  = '\0';
            write(STDOUT_FILENO, "^C\r\n", 4);
            write(STDOUT_FILENO, ctx->cprompt, strlen(ctx->cprompt));
            break;

        case K_CTRL_D:
            if (ctx->e.len == 0 && ctx->accum_len == 0) {
                write(STDOUT_FILENO, "\r\n", 2);
                raw_restore();
                InputLine eof = {NULL, 0, 1};
                ctx->on_line(eof, ctx->userdata);
                return -1;
            }
            /* Delete-forward when buffer is non-empty. */
            if (ctx->e.pos < ctx->e.len) {
                edit_delete(&ctx->e);
                redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            }
            break;

        case K_CTRL_K:
            ctx->e.len = ctx->e.pos;
            ctx->e.buf[ctx->e.len] = '\0';
            redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            break;

        case K_CTRL_U:
            memmove(ctx->e.buf, ctx->e.buf + ctx->e.pos,
                    ctx->e.len - ctx->e.pos);
            ctx->e.len -= ctx->e.pos;
            ctx->e.pos  = 0;
            ctx->e.buf[ctx->e.len] = '\0';
            redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            break;

        case K_CTRL_W:
            while (ctx->e.pos > 0 && ctx->e.buf[ctx->e.pos - 1] == ' ')
                edit_backspace(&ctx->e);
            while (ctx->e.pos > 0 && ctx->e.buf[ctx->e.pos - 1] != ' ')
                edit_backspace(&ctx->e);
            redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            break;

        case K_CTRL_L:
            write(STDOUT_FILENO, "\033[2J\033[H", 7);
            redraw_line(STDOUT_FILENO, ctx->cprompt, &ctx->e);
            break;

        case K_NONE:
        default:
            break;
        }
    }

    return 0;
}
