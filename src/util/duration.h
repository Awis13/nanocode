/*
 * duration.h — session duration string parser
 *
 * Parses human-readable duration strings like "30m", "1h", "90s", "3600".
 */

#ifndef DURATION_H
#define DURATION_H

/*
 * Parse a duration string: "30m", "1h", "90s", "3600" (bare int = seconds).
 * Returns seconds as a long, or -1 on parse error.
 * Supported suffixes: s, m, h (case-insensitive). No suffix = seconds.
 * NULL or empty string returns 0 (no timeout).
 */
long parse_duration(const char *s);

#endif /* DURATION_H */
