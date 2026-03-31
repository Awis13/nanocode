/*
 * nanocode — a zero-dependency AI coding agent in C
 *
 * Entry point. Handles signals, argument parsing, and top-level lifecycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("nanocode v0.1-dev\n");
    printf("TODO: event loop, API client, TUI\n");

    return 0;
}
