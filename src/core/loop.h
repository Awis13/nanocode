/*
 * loop.h — event loop (kqueue on macOS, epoll on Linux)
 *
 * Callers register file descriptors with a callback. The loop dispatches
 * read/write readiness events. Single-threaded; all callbacks run on the
 * calling thread.
 */

#ifndef LOOP_H
#define LOOP_H

#include <stddef.h>

/* Interest flags passed to loop_add_fd / loop_mod_fd. */
#define LOOP_READ   (1 << 0)
#define LOOP_WRITE  (1 << 1)

typedef struct Loop Loop;

/*
 * Callback invoked when the fd is ready.
 * `events` is a bitmask of LOOP_READ / LOOP_WRITE.
 * Return 0 to keep the fd registered, -1 to remove it from the loop.
 */
typedef int (*loop_cb)(int fd, int events, void *ctx);

/* Create a new event loop. Returns NULL on failure. */
Loop *loop_new(void);

/* Destroy the loop and close its internal fd. */
void  loop_free(Loop *l);

/*
 * Add `fd` to the loop with interest in `flags` events.
 * `cb` is called when ready; `ctx` is passed through.
 * Returns 0 on success, -1 on error.
 */
int   loop_add_fd(Loop *l, int fd, int flags, loop_cb cb, void *ctx);

/*
 * Modify the event interest mask for an already-registered fd.
 * Returns 0 on success, -1 if the fd was not registered.
 */
int   loop_mod_fd(Loop *l, int fd, int flags);

/*
 * Remove fd from the loop (does NOT close it).
 * Returns 0 on success, -1 if not found.
 */
int   loop_remove_fd(Loop *l, int fd);

/*
 * Run one iteration of the loop (block up to `timeout_ms` milliseconds).
 * Pass -1 for no timeout. Returns number of events dispatched, -1 on error.
 */
int   loop_step(Loop *l, int timeout_ms);

/*
 * Run the loop until `loop_stop()` is called or a fatal error occurs.
 */
void  loop_run(Loop *l);

/* Signal the loop to exit after the current iteration. */
void  loop_stop(Loop *l);

/* Make `fd` non-blocking. Returns 0 on success, -1 on error. */
int   fd_set_nonblocking(int fd);

/*
 * Schedule a one-shot callback after `delay_ms` milliseconds.
 *
 * When the timer fires, cb(timer_id, 0, ctx) is called.  The timer is
 * automatically removed from the loop after it fires (one-shot).
 *
 * On Linux the returned identifier is a timerfd file descriptor.
 * On macOS it is a synthetic integer identifier (not an fd).
 * Callers should treat it as an opaque handle and only pass it to
 * loop_cancel_timer().
 *
 * Returns a timer identifier (> 0) on success, -1 on error.
 */
int   loop_add_timer(Loop *l, int delay_ms, loop_cb cb, void *ctx);

/*
 * Cancel a pending timer.  Safe to call if the timer has already fired.
 * `timer_id` must be a value returned by loop_add_timer().
 */
void  loop_cancel_timer(Loop *l, int timer_id);

#endif /* LOOP_H */
