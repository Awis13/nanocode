/*
 * spinner.h — braille breathing spinner for API wait state
 *
 * Shows a rotating braille glyph while waiting for the first SSE token.
 * Designed for the 16 ms LOOP_STREAMING cadence (~60 fps).
 *
 * Usage:
 *   Spinner sp;
 *   spinner_start(&sp, STDOUT_FILENO);
 *   // ... each loop iteration at 16 ms:
 *   spinner_tick(&sp);
 *   // ... when first token arrives:
 *   spinner_stop(&sp);
 */

#ifndef SPINNER_H
#define SPINNER_H

#include <stdint.h>

typedef struct {
    int      fd;
    int      frame;      /* current animation frame index         */
    int      shown;      /* non-zero while glyph is on-screen     */
    int      color;      /* non-zero if 256-colour available       */
    int64_t  start_ns;   /* CLOCK_MONOTONIC at spinner_start()     */
    int64_t  last_ns;    /* last tick timestamp                    */
} Spinner;

/*
 * Initialise *s and record the start time.  `color` should be non-zero
 * when the terminal supports 256-colour (pass Renderer.is_256color or
 * detect from TERM/COLORTERM yourself).
 * Does not write to fd yet.
 */
void spinner_start(Spinner *s, int fd, int color);

/*
 * Advance the spinner by one frame.  On the first call after 80 ms have
 * elapsed since spinner_start(), the spinner appears on-screen.
 * Each subsequent call updates the glyph in-place (carriage-return trick).
 * Safe to call at 60 fps.
 */
void spinner_tick(Spinner *s);

/*
 * Erase the spinner glyph from the terminal and reset *s.
 * Call before the first renderer token so the spinner does not linger.
 */
void spinner_stop(Spinner *s);

#endif /* SPINNER_H */
