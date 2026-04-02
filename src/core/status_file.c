/*
 * status_file.c — atomically written agent status file
 *
 * Writes JSON to <path>.tmp then rename()s it for atomic updates.
 * Uses stack buffers only — no arena, no heap.
 */

#include "status_file.h"
#include "../util/json.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Maximum length of the tmp path suffix ".tmp" */
#define TMP_SUFFIX      ".tmp"
#define TMP_SUFFIX_LEN  4

void status_file_write(const char *path, const StatusInfo *info)
{
    if (!path || !info)
        return;

    /* Build the JSON into a 2KB stack buffer. */
    char buf[2048];
    size_t cap = sizeof(buf);
    size_t pos = 0;

    /* { */
    if (pos < cap) buf[pos] = '{'; pos++;

    /* "pid": <N> */
    {
        char num[32];
        int n = snprintf(num, sizeof(num), "\"pid\":%ld,", (long)info->pid);
        if (n > 0 && pos + (size_t)n < cap) {
            memcpy(buf + pos, num, (size_t)n);
        }
        pos += (size_t)(n > 0 ? n : 0);
    }

    /* "state": ... */
    if (pos + 9 < cap) { memcpy(buf + pos, "\"state\":", 8); }
    pos += 8;
    pos = json_escape_str(buf, cap, pos, info->state);
    if (pos < cap) buf[pos] = ','; pos++;

    /* "task": ... */
    if (pos + 8 < cap) { memcpy(buf + pos, "\"task\":", 7); }
    pos += 7;
    pos = json_escape_str(buf, cap, pos, info->task);
    if (pos < cap) buf[pos] = ','; pos++;

    /* "started_at": ... */
    if (pos + 14 < cap) { memcpy(buf + pos, "\"started_at\":", 13); }
    pos += 13;
    pos = json_escape_str(buf, cap, pos, info->started_at);
    if (pos < cap) buf[pos] = ','; pos++;

    /* "last_action": ... */
    if (pos + 15 < cap) { memcpy(buf + pos, "\"last_action\":", 14); }
    pos += 14;
    pos = json_escape_str(buf, cap, pos, info->last_action);
    if (pos < cap) buf[pos] = ','; pos++;

    /* "tool_calls": <N> */
    {
        char num[32];
        int n = snprintf(num, sizeof(num), "\"tool_calls\":%d", info->tool_calls);
        if (n > 0 && pos + (size_t)n < cap) {
            memcpy(buf + pos, num, (size_t)n);
        }
        pos += (size_t)(n > 0 ? n : 0);
    }

    /* } */
    if (pos < cap) buf[pos] = '}'; pos++;
    /* NUL terminate */
    if (pos < cap) buf[pos] = '\0'; else buf[cap - 1] = '\0';

    /* Build tmp path on stack. */
    size_t plen = strlen(path);
    if (plen + TMP_SUFFIX_LEN + 1 > 4096) {
        /* Path too long — silently skip. */
        return;
    }
    char tmp_path[4096];
    memcpy(tmp_path, path, plen);
    memcpy(tmp_path + plen, TMP_SUFFIX, TMP_SUFFIX_LEN + 1);

    FILE *f = fopen(tmp_path, "w");
    if (!f)
        return;

    size_t len = pos < cap ? pos : cap - 1;
    fwrite(buf, 1, len, f);
    fclose(f);

    rename(tmp_path, path);
}

void status_file_remove(const char *path)
{
    if (!path)
        return;
    unlink(path);
}
