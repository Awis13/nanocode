/*
 * duration.c — session duration string parser (CMP-212)
 */

#include "duration.h"

#include <ctype.h>
#include <stdlib.h>

long parse_duration(const char *s)
{
    if (!s || !*s)
        return 0;

    char *end;
    long val = strtol(s, &end, 10);

    if (end == s || val < 0)
        return -1;

    if (*end == '\0')
        return val;   /* bare integer — seconds */

    /* One suffix character, nothing after it. */
    if (end[1] != '\0')
        return -1;

    int suffix = tolower((unsigned char)*end);
    switch (suffix) {
        case 's': return val;
        case 'm': return val * 60;
        case 'h': return val * 3600;
        default:  return -1;
    }
}
