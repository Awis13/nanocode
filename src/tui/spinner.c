/*
 * spinner.c — braille breathing spinner for API wait state
 *
 * Displays a rotating braille glyph while waiting for the first SSE token.
 * Tuned for the 16 ms LOOP_STREAMING cadence so it animates at ~60 fps
 * but only updates visually every ~80 ms (5 frames) to avoid flicker.
 *
 * Braille frames: ⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏  (U+280B … U+280F range)
 * Each glyph is exactly 3 UTF-8 bytes.
 *
 * Colour: Catppuccin Mocha Sapphire (\033[38;5;117m) when 256-colour,
 * plain cyan (\033[36m) otherwise.
 */

#include "tui/spinner.h"

#include <time.h>
#include <unistd.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Braille spinner frames (10 glyphs, each 3 UTF-8 bytes)
 * ---------------------------------------------------------------------- */

#define NFRAMES 10
static const char BRAILLE[NFRAMES][4] = {
    "\xe2\xa0\x8b",   /* ⠋ */
    "\xe2\xa0\x99",   /* ⠙ */
    "\xe2\xa0\xb9",   /* ⠹ */
    "\xe2\xa0\xb8",   /* ⠸ */
    "\xe2\xa0\xbc",   /* ⠼ */
    "\xe2\xa0\xb4",   /* ⠴ */
    "\xe2\xa0\xa6",   /* ⠦ */
    "\xe2\xa0\xa7",   /* ⠧ */
    "\xe2\xa0\x87",   /* ⠇ */
    "\xe2\xa0\x8f",   /* ⠏ */
};

/* Catppuccin Mocha Sapphire ≈ 256-colour index 117 */
#define COLOR_256  "\033[38;5;117m"
#define COLOR_16   "\033[36m"         /* cyan fallback */
#define COLOR_RST  "\033[0m"

/* Update visual frame every 80 ms (5 × 16 ms loop ticks). */
#define FRAME_INTERVAL_NS  80000000LL   /* 80 ms */
/* Initial delay before showing spinner. */
#define SHOW_DELAY_NS      80000000LL   /* 80 ms */

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void fd_write(int fd, const char *s, size_t n)
{
    if (n > 0) (void)write(fd, s, n);
}

static void fd_puts(int fd, const char *s)
{
    fd_write(fd, s, strlen(s));
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void spinner_start(Spinner *s, int fd, int color)
{
    s->fd       = fd;
    s->frame    = 0;
    s->shown    = 0;
    s->color    = color;
    s->start_ns = now_ns();
    s->last_ns  = s->start_ns;
}

void spinner_tick(Spinner *s)
{
    if (!s) return;

    int64_t now = now_ns();

    /* Wait for the initial delay before appearing. */
    if (now - s->start_ns < SHOW_DELAY_NS) return;

    /* Throttle frame updates to ~12.5 fps (80 ms) to avoid flicker. */
    if (s->shown && (now - s->last_ns < FRAME_INTERVAL_NS)) return;

    s->last_ns = now;

    char buf[32];
    int  pos = 0;

    if (s->shown) {
        /* Overwrite the previous glyph: CR, then re-emit. */
        buf[pos++] = '\r';
    }

    /* Colour */
    const char *col = s->color ? COLOR_256 : COLOR_16;
    int clen = (int)strlen(col);
    memcpy(buf + pos, col, (size_t)clen); pos += clen;

    /* Braille glyph (3 bytes) */
    memcpy(buf + pos, BRAILLE[s->frame], 3); pos += 3;

    /* Trailing space + reset so the next character is clean */
    buf[pos++] = ' ';
    const char *rst = COLOR_RST;
    int rlen = (int)strlen(rst);
    memcpy(buf + pos, rst, (size_t)rlen); pos += rlen;

    fd_write(s->fd, buf, (size_t)pos);

    s->frame = (s->frame + 1) % NFRAMES;
    s->shown = 1;
}

void spinner_stop(Spinner *s)
{
    if (!s) return;
    if (s->shown) {
        /* Erase: CR, two spaces, CR — clears the 2-char glyph+space. */
        fd_puts(s->fd, "\r  \r");
        s->shown = 0;
    }
    s->frame = 0;
}
