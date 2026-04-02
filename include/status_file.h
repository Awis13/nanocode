/*
 * status_file.h — atomically written agent status file
 *
 * Provides a machine-readable snapshot of what the agent is doing.
 * Written to <path>.tmp then rename()d for atomic updates.
 */

#ifndef STATUS_FILE_H
#define STATUS_FILE_H

#include <sys/types.h>

typedef struct {
    pid_t  pid;
    char  *state;         /* "idle", "working", "waiting_input" */
    char  *task;          /* current prompt or last prompt */
    char  *started_at;    /* ISO 8601 UTC */
    const char *last_action; /* last tool name invoked */
    int    tool_calls;    /* running count since startup */
} StatusInfo;

/* Write status as JSON to path (via atomic tmp+rename). */
void status_file_write(const char *path, const StatusInfo *info);

/* Remove the status file (called on clean exit). */
void status_file_remove(const char *path);

#endif /* STATUS_FILE_H */
