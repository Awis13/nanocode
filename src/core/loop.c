/*
 * loop.c — event loop implementation
 *
 * Uses kqueue on macOS/BSD and epoll on Linux.
 */

#include "loop.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

/* -------------------------------------------------------------------------
 * Per-fd entry
 * ---------------------------------------------------------------------- */

#define MAX_FDS 256

typedef struct {
    int      fd;
    int      flags;   /* LOOP_READ | LOOP_WRITE */
    loop_cb  cb;
    void    *ctx;
} FdEntry;

/* -------------------------------------------------------------------------
 * Platform-specific backend
 * ---------------------------------------------------------------------- */

/* Flag to mark timer-backed entries (no real fd). */
#define LOOP_TIMER  (1 << 2)

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>

/* Synthetic identifiers for kqueue EVFILT_TIMER (not real fds).
 * Start well above any plausible fd number. */
static int g_timer_next_id = 0x1000000;

struct Loop {
    int      kq;
    int      running;
    FdEntry  entries[MAX_FDS];
    int      nentries;
};

Loop *loop_new(void)
{
    Loop *l = calloc(1, sizeof(Loop));
    if (!l)
        return NULL;
    l->kq = kqueue();
    if (l->kq < 0) {
        free(l);
        return NULL;
    }
    l->running = 0;
    l->nentries = 0;
    return l;
}

void loop_free(Loop *l)
{
    close(l->kq);
    free(l);
}

static FdEntry *find_entry(Loop *l, int fd)
{
    for (int i = 0; i < l->nentries; i++)
        if (l->entries[i].fd == fd)
            return &l->entries[i];
    return NULL;
}

static int kq_update(Loop *l, int fd, int old_flags, int new_flags)
{
    struct kevent changes[2];
    int nchanges = 0;

    /* Read interest */
    if ((new_flags & LOOP_READ) && !(old_flags & LOOP_READ)) {
        EV_SET(&changes[nchanges++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    } else if (!(new_flags & LOOP_READ) && (old_flags & LOOP_READ)) {
        EV_SET(&changes[nchanges++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    }

    /* Write interest */
    if ((new_flags & LOOP_WRITE) && !(old_flags & LOOP_WRITE)) {
        EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    } else if (!(new_flags & LOOP_WRITE) && (old_flags & LOOP_WRITE)) {
        EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    }

    if (nchanges == 0)
        return 0;

    return kevent(l->kq, changes, nchanges, NULL, 0, NULL);
}

int loop_add_fd(Loop *l, int fd, int flags, loop_cb cb, void *ctx)
{
    if (l->nentries >= MAX_FDS) {
        fprintf(stderr, "loop: MAX_FDS reached\n");
        return -1;
    }
    if (kq_update(l, fd, 0, flags) < 0)
        return -1;

    FdEntry *e  = &l->entries[l->nentries++];
    e->fd    = fd;
    e->flags = flags;
    e->cb    = cb;
    e->ctx   = ctx;
    return 0;
}

int loop_mod_fd(Loop *l, int fd, int flags)
{
    FdEntry *e = find_entry(l, fd);
    if (!e)
        return -1;
    if (kq_update(l, fd, e->flags, flags) < 0)
        return -1;
    e->flags = flags;
    return 0;
}

int loop_remove_fd(Loop *l, int fd)
{
    for (int i = 0; i < l->nentries; i++) {
        if (l->entries[i].fd == fd) {
            kq_update(l, fd, l->entries[i].flags, 0);
            /* compact the array */
            l->entries[i] = l->entries[--l->nentries];
            return 0;
        }
    }
    return -1;
}

int loop_step(Loop *l, int timeout_ms)
{
    struct kevent events[32];
    struct timespec ts, *tsp = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(l->kq, NULL, 0, events, 32, tsp);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        perror("kevent");
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int ev_fd = (int)(uintptr_t)events[i].ident;
        int ev    = 0;

        if (events[i].filter == EVFILT_TIMER) {
            /* One-shot timer fired — call callback and remove entry. */
            FdEntry *e = find_entry(l, ev_fd);
            if (e)
                e->cb(ev_fd, 0, e->ctx);
            loop_remove_fd(l, ev_fd);
            continue;
        }

        if (events[i].filter == EVFILT_READ)  ev |= LOOP_READ;
        if (events[i].filter == EVFILT_WRITE) ev |= LOOP_WRITE;

        FdEntry *e = find_entry(l, ev_fd);
        if (!e)
            continue;

        int r = e->cb(ev_fd, ev, e->ctx);
        if (r < 0)
            loop_remove_fd(l, ev_fd);
    }
    return n;
}

int loop_add_timer(Loop *l, int delay_ms, loop_cb cb, void *ctx)
{
    if (l->nentries >= MAX_FDS)
        return -1;

    int id = g_timer_next_id++;
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)id, EVFILT_TIMER,
           EV_ADD | EV_ONESHOT, NOTE_USECONDS, (intptr_t)delay_ms * 1000, NULL);
    if (kevent(l->kq, &kev, 1, NULL, 0, NULL) < 0)
        return -1;

    FdEntry *e = &l->entries[l->nentries++];
    e->fd    = id;
    e->flags = LOOP_TIMER;
    e->cb    = cb;
    e->ctx   = ctx;
    return id;
}

void loop_cancel_timer(Loop *l, int timer_id)
{
    /* Remove from kqueue (EV_ONESHOT may have already removed it; ignore err) */
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)timer_id, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    kevent(l->kq, &kev, 1, NULL, 0, NULL);
    loop_remove_fd(l, timer_id);
}

#else  /* Linux — epoll */

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <stdint.h>

struct Loop {
    int      epfd;
    int      running;
    FdEntry  entries[MAX_FDS];
    int      nentries;
};

Loop *loop_new(void)
{
    Loop *l = calloc(1, sizeof(Loop));
    if (!l)
        return NULL;
    l->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (l->epfd < 0) {
        free(l);
        return NULL;
    }
    return l;
}

void loop_free(Loop *l)
{
    close(l->epfd);
    free(l);
}

static FdEntry *find_entry(Loop *l, int fd)
{
    for (int i = 0; i < l->nentries; i++)
        if (l->entries[i].fd == fd)
            return &l->entries[i];
    return NULL;
}

static uint32_t flags_to_epoll(int flags)
{
    uint32_t ev = EPOLLET;
    if (flags & LOOP_READ)  ev |= EPOLLIN;
    if (flags & LOOP_WRITE) ev |= EPOLLOUT;
    return ev;
}

int loop_add_fd(Loop *l, int fd, int flags, loop_cb cb, void *ctx)
{
    if (l->nentries >= MAX_FDS)
        return -1;

    struct epoll_event ev = { .events = flags_to_epoll(flags), .data.fd = fd };
    if (epoll_ctl(l->epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return -1;

    FdEntry *e  = &l->entries[l->nentries++];
    e->fd    = fd;
    e->flags = flags;
    e->cb    = cb;
    e->ctx   = ctx;
    return 0;
}

int loop_mod_fd(Loop *l, int fd, int flags)
{
    FdEntry *e = find_entry(l, fd);
    if (!e)
        return -1;
    struct epoll_event ev = { .events = flags_to_epoll(flags), .data.fd = fd };
    if (epoll_ctl(l->epfd, EPOLL_CTL_MOD, fd, &ev) < 0)
        return -1;
    e->flags = flags;
    return 0;
}

int loop_remove_fd(Loop *l, int fd)
{
    for (int i = 0; i < l->nentries; i++) {
        if (l->entries[i].fd == fd) {
            epoll_ctl(l->epfd, EPOLL_CTL_DEL, fd, NULL);
            l->entries[i] = l->entries[--l->nentries];
            return 0;
        }
    }
    return -1;
}

int loop_step(Loop *l, int timeout_ms)
{
    struct epoll_event events[32];
    int n = epoll_wait(l->epfd, events, 32, timeout_ms);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        perror("epoll_wait");
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int ev_fd = events[i].data.fd;
        int ev    = 0;
        if (events[i].events & EPOLLIN)  ev |= LOOP_READ;
        if (events[i].events & EPOLLOUT) ev |= LOOP_WRITE;

        FdEntry *e = find_entry(l, ev_fd);
        if (!e)
            continue;

        if (e->flags & LOOP_TIMER) {
            /* Drain the timerfd so it doesn't stay readable. */
            uint64_t exp;
            (void)read(ev_fd, &exp, sizeof(exp));
            e->cb(ev_fd, 0, e->ctx);
            /* One-shot: remove from epoll and close the timerfd. */
            loop_remove_fd(l, ev_fd);
            close(ev_fd);
            continue;
        }

        int r = e->cb(ev_fd, ev, e->ctx);
        if (r < 0)
            loop_remove_fd(l, ev_fd);
    }
    return n;
}

int loop_add_timer(Loop *l, int delay_ms, loop_cb cb, void *ctx)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
        return -1;

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec  = delay_ms / 1000;
    its.it_value.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        close(tfd);
        return -1;
    }

    if (loop_add_fd(l, tfd, LOOP_READ | LOOP_TIMER, cb, ctx) < 0) {
        close(tfd);
        return -1;
    }
    return tfd;
}

void loop_cancel_timer(Loop *l, int timer_fd)
{
    loop_remove_fd(l, timer_fd);
    close(timer_fd);
}

#endif  /* platform */

/* -------------------------------------------------------------------------
 * Common loop_run / loop_stop
 * ---------------------------------------------------------------------- */

void loop_run(Loop *l)
{
    l->running = 1;
    while (l->running) {
        /*
         * Zombie reaping sweep (CMP-183).
         *
         * bash.c reaps its own children via blocking waitpid() before
         * returning to the loop, so zombies here are rare.  This sweep
         * catches any that slip through — e.g. after SIGKILL of a timed-out
         * child process group — without blocking the loop.
         *
         * SIGCHLD is intentionally left at SIG_DFL so that bash.c's
         * explicit waitpid() can still collect exit status; this non-blocking
         * sweep is a safety net only.
         */
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;

        if (loop_step(l, 100) < 0)
            break;
    }
}

void loop_stop(Loop *l)
{
    l->running = 0;
}

/* -------------------------------------------------------------------------
 * fd_set_nonblocking
 * ---------------------------------------------------------------------- */

int fd_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
