/*
 * config.c — nanocode configuration system (CMP-141)
 *
 * Minimal TOML parser: string / int / bool values, sections.
 * No arrays, no tables-of-tables. Invalid lines are warned and skipped.
 * All storage lives in the caller's Arena (persistent, no individual frees).
 */

#include "../../include/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * Internal limits
 * ---------------------------------------------------------------------- */

#define CFG_MAX_ENTRIES  64
#define CFG_KEY_MAX      64
#define CFG_VAL_MAX      512
#define CFG_LINE_MAX     1024
#define CFG_FILE_MAX     (64 * 1024)   /* max config file size: 64 KB */

/* -------------------------------------------------------------------------
 * Compiled-in defaults
 * ---------------------------------------------------------------------- */

static const struct { const char *key; const char *val; } s_defaults[] = {
    { "provider.api_key",        ""                              },
    { "provider.base_url",       "https://api.anthropic.com"    },
    { "provider.model",          "claude-opus-4-6"              },
    { "provider.timeout_ms",     "30000"                        },
    { "sandbox.enabled",         "true"                         },
    { "sandbox.profile",         "strict"                       },
    { "ui.theme",                "dark"                         },
    { "ui.word_wrap",            "true"                         },
    { "ui.stream_delay_ms",      "0"                            },
    { "system_prompt.append",    ""                             },
    { NULL, NULL }
};

/* Default config file content written on first run. */
static const char s_default_toml[] =
    "# nanocode configuration\n"
    "# ~/.nanocode/config.toml\n"
    "\n"
    "[provider]\n"
    "api_key      = \"\"\n"
    "base_url     = \"https://api.anthropic.com\"\n"
    "model        = \"claude-opus-4-6\"\n"
    "timeout_ms   = 30000\n"
    "\n"
    "[sandbox]\n"
    "enabled      = true\n"
    "profile      = \"strict\"\n"
    "\n"
    "[ui]\n"
    "theme        = \"dark\"\n"
    "word_wrap    = true\n"
    "stream_delay_ms = 0\n"
    "\n"
    "[system_prompt]\n"
    "append       = \"\"\n";

/* -------------------------------------------------------------------------
 * Config struct (arena-allocated; entries are embedded)
 * ---------------------------------------------------------------------- */

struct Config {
    struct {
        char key[CFG_KEY_MAX];
        char val[CFG_VAL_MAX];
    } entries[CFG_MAX_ENTRIES];
    int count;
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Trim leading ASCII whitespace in-place; returns pointer into s. */
static char *ltrim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Trim trailing ASCII whitespace + CR in-place. */
static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r'))
        s[--n] = '\0';
}

/* Store or overwrite a key/value pair in the config. */
static void cfg_set(struct Config *cfg, const char *key, const char *val)
{
    /* Overwrite existing entry if present. */
    for (int i = 0; i < cfg->count; i++) {
        if (strncmp(cfg->entries[i].key, key, CFG_KEY_MAX) == 0) {
            strncpy(cfg->entries[i].val, val, CFG_VAL_MAX - 1);
            cfg->entries[i].val[CFG_VAL_MAX - 1] = '\0';
            return;
        }
    }
    /* Append new entry. */
    if (cfg->count >= CFG_MAX_ENTRIES) {
        fprintf(stderr, "config: warning: entry limit reached, skipping key: %s\n", key);
        return;
    }
    strncpy(cfg->entries[cfg->count].key, key, CFG_KEY_MAX - 1);
    cfg->entries[cfg->count].key[CFG_KEY_MAX - 1] = '\0';
    strncpy(cfg->entries[cfg->count].val, val, CFG_VAL_MAX - 1);
    cfg->entries[cfg->count].val[CFG_VAL_MAX - 1] = '\0';
    cfg->count++;
}

/* -------------------------------------------------------------------------
 * TOML parser (~200 LOC)
 *
 * Parses a null-terminated TOML text into `cfg`.
 * Supports: sections [name], key = value, string/int/bool values.
 * Skips: blank lines, # comments, unrecognised syntax (with a warning).
 * ---------------------------------------------------------------------- */

static void parse_toml(struct Config *cfg, const char *text)
{
    char section[64] = "";
    char line[CFG_LINE_MAX];
    const char *p = text;
    int lineno = 0;

    while (*p) {
        /* ---- Extract one line ---- */
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= CFG_LINE_MAX)
            len = CFG_LINE_MAX - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + len;
        lineno++;

        char *s = ltrim(line);
        rtrim(s);

        /* Skip blank lines and comments. */
        if (*s == '\0' || *s == '#')
            continue;

        /* ---- Section header: [name] ---- */
        if (*s == '[') {
            char *close = strchr(s + 1, ']');
            if (!close) {
                fprintf(stderr, "config: warning: line %d: malformed section header\n",
                        lineno);
                continue;
            }
            size_t slen = (size_t)(close - (s + 1));
            if (slen == 0 || slen >= sizeof(section)) {
                fprintf(stderr, "config: warning: line %d: section name too long or empty\n",
                        lineno);
                continue;
            }
            memcpy(section, s + 1, slen);
            section[slen] = '\0';
            /* Trim whitespace from section name. */
            char *ts = ltrim(section);
            rtrim(ts);
            memmove(section, ts, strlen(ts) + 1);
            continue;
        }

        /* ---- Key = Value ---- */
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "config: warning: line %d: skipping invalid line: %s\n",
                    lineno, s);
            continue;
        }

        /* Key: everything before '=', trimmed. */
        *eq = '\0';
        rtrim(s);
        char *key = ltrim(s);
        if (*key == '\0') {
            fprintf(stderr, "config: warning: line %d: empty key\n", lineno);
            continue;
        }

        /* Value: everything after '=', trimmed, comment stripped. */
        char *v = ltrim(eq + 1);

        /* Strip inline comment — only outside double-quotes. */
        int in_q = 0;
        for (char *vc = v; *vc; vc++) {
            if (*vc == '"')        in_q = !in_q;
            if (*vc == '#' && !in_q) { *vc = '\0'; break; }
        }
        rtrim(v);

        /* Strip surrounding double-quotes from string values. */
        size_t vlen = strlen(v);
        if (vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"') {
            v[vlen - 1] = '\0';
            v++;
        }

        /* Build dotted key. */
        char full_key[CFG_KEY_MAX];
        if (section[0])
            snprintf(full_key, sizeof(full_key), "%s.%s", section, key);
        else
            snprintf(full_key, sizeof(full_key), "%s", key);

        cfg_set(cfg, full_key, v);
    }
}

/* -------------------------------------------------------------------------
 * Default-lookup helpers
 * ---------------------------------------------------------------------- */

static const char *default_val(const char *key)
{
    for (int i = 0; s_defaults[i].key; i++) {
        if (strcmp(s_defaults[i].key, key) == 0)
            return s_defaults[i].val;
    }
    return "";
}

static const char *cfg_get_raw(const struct Config *cfg, const char *key)
{
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0)
            return cfg->entries[i].val;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

Config *config_load_path(Arena *arena, const char *path)
{
    if (!arena || !path)
        return NULL;

    struct Config *cfg = arena_alloc(arena, sizeof(struct Config));
    if (!cfg)
        return NULL;
    memset(cfg, 0, sizeof(struct Config));

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Write default config if the open failed (likely: file absent). */
        if (errno == ENOENT) {
            f = fopen(path, "w");
            if (f) {
                fputs(s_default_toml, f);
                fclose(f);
            }
        }
        /* Return config with defaults (entries count == 0, getters fall back). */
        return cfg;
    }

    /* Read entire file into arena. */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return cfg;
    }
    long fsz = ftell(f);
    rewind(f);

    if (fsz <= 0 || fsz > CFG_FILE_MAX) {
        if (fsz > CFG_FILE_MAX)
            fprintf(stderr, "config: warning: file too large (%ld bytes), using defaults\n",
                    fsz);
        fclose(f);
        return cfg;
    }

    char *buf = arena_alloc(arena, (size_t)fsz + 1);
    if (!buf) {
        fclose(f);
        return cfg;
    }

    size_t nread = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    buf[nread] = '\0';

    parse_toml(cfg, buf);
    return cfg;
}

Config *config_load(Arena *arena)
{
    if (!arena)
        return NULL;

    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "config: warning: HOME not set, using empty config\n");
        struct Config *cfg = arena_alloc(arena, sizeof(struct Config));
        if (cfg) memset(cfg, 0, sizeof(struct Config));
        return cfg;
    }

    /* Ensure ~/.nanocode/ exists. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.nanocode", home);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        fprintf(stderr, "config: warning: could not create %s: %s\n",
                dir, strerror(errno));

    char path[512];
    snprintf(path, sizeof(path), "%s/.nanocode/config.toml", home);
    return config_load_path(arena, path);
}

const char *config_get_str(const Config *cfg, const char *key)
{
    if (!cfg || !key) return "";
    const char *v = cfg_get_raw(cfg, key);
    return v ? v : default_val(key);
}

int config_get_int(const Config *cfg, const char *key)
{
    const char *v = config_get_str(cfg, key);
    if (!v || !*v) return 0;
    return (int)strtol(v, NULL, 10);
}

int config_get_bool(const Config *cfg, const char *key)
{
    const char *v = config_get_str(cfg, key);
    if (!v || !*v) return 0;
    if (strcmp(v, "true")  == 0 || strcmp(v, "1") == 0) return 1;
    if (strcmp(v, "false") == 0 || strcmp(v, "0") == 0) return 0;
    /* Any non-zero integer counts as truthy. */
    char *end;
    long n = strtol(v, &end, 10);
    if (end != v) return n != 0;
    return 0;
}
