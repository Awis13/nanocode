/*
 * config.c — nanocode configuration system (CMP-141, CMP-187)
 *
 * Minimal TOML parser: string / int / bool values, sections.
 * No arrays, no tables-of-tables. Invalid lines are warned and skipped.
 * All storage lives in the caller's Arena (persistent, no individual frees).
 *
 * CMP-187 additions:
 *   - Six new config sections: rendering, theme, layout, behavior, keys, performance
 *   - config_set()     — mutate a live Config at runtime
 *   - config_save()    — persist current config to a TOML file with comments
 *   - config_cmd_set() — parse ":set key value" input and call config_set
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

#define CFG_MAX_ENTRIES  80
#define CFG_KEY_MAX      64
#define CFG_VAL_MAX      512
#define CFG_LINE_MAX     1024
#define CFG_FILE_MAX     (64 * 1024)   /* max config file size: 64 KB */

/* -------------------------------------------------------------------------
 * Compiled-in defaults
 * ---------------------------------------------------------------------- */

static const struct { const char *key; const char *val; } s_defaults[] = {
    /* [provider] */
    { "provider.type",                 "claude"                    },
    { "provider.api_key",              ""                          },
    { "provider.base_url",             "api.anthropic.com"         },
    { "provider.port",                 "0"                         },
    { "provider.model",                "claude-opus-4-6"           },
    { "provider.timeout_ms",           "30000"                     },
    /* [sandbox] */
    { "sandbox.enabled",               "true"                      },
    { "sandbox.profile",               "strict"                    },
    { "sandbox.allowed_paths",         ""                          },
    { "sandbox.allowed_commands",      ""                          },
    { "sandbox.denied_commands",       ""                          },
    { "sandbox.network",               "false"                     },
    { "sandbox.max_file_size",         "10485760"                  },
    /* [ui] */
    { "ui.theme",                      "dark"                      },
    { "ui.word_wrap",                  "true"                      },
    { "ui.stream_delay_ms",            "0"                         },
    /* [system_prompt] */
    { "system_prompt.append",          ""                          },
    /* [rendering] */
    { "rendering.target_fps",          "60"                        },
    { "rendering.frame_batch_ms",      "16"                        },
    { "rendering.scroll_mode",         "smooth"                    },
    /* [theme] */
    { "theme.accent_color",            "cyan"                      },
    { "theme.diff_add_color",          "green"                     },
    { "theme.diff_rm_color",           "red"                       },
    { "theme.syntax_theme",            "monokai"                   },
    { "theme.true_color",              "true"                      },
    /* [layout] */
    { "layout.panel_split",            "horizontal"                },
    { "layout.status_bar_position",    "bottom"                    },
    { "layout.max_width",              "0"                         },
    { "layout.padding",                "1"                         },
    /* [behavior] */
    { "behavior.auto_approve_tools",   "false"                     },
    { "behavior.confirm_destructive",  "true"                      },
    { "behavior.max_context_tokens",   "100000"                    },
    /* [keys] */
    { "keys.submit",                   "enter"                     },
    { "keys.cancel",                   "ctrl-c"                    },
    { "keys.scroll_up",                "ctrl-u"                    },
    { "keys.scroll_down",              "ctrl-d"                    },
    /* [pet] */
    { "pet",                           ""                          },
    /* [audit] */
    { "audit.enabled",             "true"                     },
    { "audit.path",                ""                         },
    { "audit.max_size_bytes",      "10485760"                 },
    { "audit.max_files",           "5"                        },
    /* [session] */
    { "session.mode",                  ""                          },
    { "session.max_files_created",     "50"                        },
    { "session.timeout",               ""                          },
    /* [performance] */
    { "performance.idle_timeout_ms",   "5000"                      },
    { "performance.max_output_lines",  "10000"                     },
    { "performance.history_limit_mb",  "10"                        },
    { NULL, NULL }
};

/*
 * Fully-commented default config written on first run (CMP-187).
 */
/* Split into two halves so each literal stays under the C99 4095-byte
 * minimum portability limit (-Woverlength-strings).  Both halves are
 * written by config_write_defaults() below. */
static const char s_default_toml_a[] =
    "# nanocode configuration\n"
    "# ~/.nanocode/config.toml\n"
    "#\n"
    "# Edit this file to customise nanocode.\n"
    "# Changes take effect on the next startup unless noted as live-reloadable.\n"
    "# Runtime changes: use  :set key value  in the prompt.\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[provider]\n"
    "type         = \"claude\"               # Provider: claude | openai | ollama\n"
    "api_key      = \"\"                     # API key (or set ANTHROPIC_API_KEY)\n"
    "base_url     = \"api.anthropic.com\"    # API hostname (no scheme; use port for Ollama)\n"
    "port         = 0                      # Port override (0 = default: 443 for claude, 11434 for openai/ollama)\n"
    "model        = \"claude-opus-4-6\"      # Model ID\n"
    "timeout_ms   = 30000                  # Request timeout in milliseconds\n"
    "#\n"
    "# -- Gemma 4 via Ollama (128K context window) --\n"
    "# type     = \"ollama\"\n"
    "# base_url = \"localhost\"\n"
    "# model    = \"gemma4\"\n"
    "# port     = 11434\n"
    "#\n"
    "# -- Gemma 4 via vLLM / LM Studio (OpenAI-compatible) --\n"
    "# type     = \"openai\"\n"
    "# base_url = \"localhost\"\n"
    "# model    = \"google/gemma-4\"\n"
    "# port     = 8000\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[sandbox]\n"
    "enabled          = true                # Enable tool sandboxing\n"
    "profile          = \"strict\"           # Sandbox profile: strict | permissive | custom\n"
    "allowed_paths    = \"\"                 # Colon-separated absolute paths (required for custom)\n"
    "allowed_commands = \"\"                 # Colon-separated command basenames (required for custom)\n"
    "denied_commands  = \"\"                 # Colon-separated command basenames to always block\n"
    "network          = false               # Allow outbound network from tools\n"
    "max_file_size    = 10485760            # Max bytes per file write (10 MB default)\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[ui]\n"
    "theme        = \"dark\"                  # Colour theme: dark | light | auto\n"
    "word_wrap    = true                    # Wrap long lines in the output panel\n"
    "stream_delay_ms = 0                   # Extra delay between streamed tokens (ms)\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[system_prompt]\n"
    "append       = \"\"                      # Extra text appended to the system prompt\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[rendering]\n"
    "target_fps      = 60                  # Target render frames per second\n"
    "frame_batch_ms  = 16                  # Max ms to batch incoming tokens before a frame\n"
    "scroll_mode     = \"smooth\"            # Scroll mode: smooth | jump\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[theme]\n"
    "accent_color    = \"cyan\"              # Accent colour (ANSI name or #rrggbb)\n"
    "diff_add_color  = \"green\"             # Colour for added diff lines\n"
    "diff_rm_color   = \"red\"              # Colour for removed diff lines\n"
    "syntax_theme    = \"monokai\"           # Syntax highlighting theme name\n"
    "true_color      = true                # 24-bit colour (false for 256-color terminals)\n";

static const char s_default_toml_b[] =
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[layout]\n"
    "panel_split          = \"horizontal\"  # Panel split direction: horizontal | vertical\n"
    "status_bar_position  = \"bottom\"      # Status bar placement: top | bottom\n"
    "max_width            = 0             # Max output width in columns (0 = terminal width)\n"
    "padding              = 1             # Inner padding in character cells\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[behavior]\n"
    "auto_approve_tools   = false         # Skip confirmation prompts for all tools\n"
    "confirm_destructive  = true          # Extra confirmation for destructive tool calls\n"
    "max_context_tokens   = 100000        # Hard cap on context window tokens (set to 128000 for Gemma 4 / full 128K context)\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[keys]\n"
    "submit       = \"enter\"               # Key to submit input (enter | ctrl-j)\n"
    "cancel       = \"ctrl-c\"              # Key to cancel in-progress request\n"
    "scroll_up    = \"ctrl-u\"              # Key to scroll output up\n"
    "scroll_down  = \"ctrl-d\"              # Key to scroll output down\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[performance]\n"
    "idle_timeout_ms   = 5000             # Idle loop sleep time in milliseconds\n"
    "max_output_lines  = 10000            # Max lines kept in the output ring buffer\n"
    "history_limit_mb  = 10              # Max size of the input history file in MB\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "[session]\n"
    "max_files_created = 50              # Max new files an agent may create per session\n"
    "timeout           = \"\"             # Session auto-terminate duration: 30m, 1h, 90s, or empty for none\n"
    "\n"
    "# ---------------------------------------------------------------------------\n"
    "# pet = \"cat\"                       # Active pet: cat | crab | dog | off\n"
    "#                                   # (empty = auto-pick on first run)\n";

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

static char *ltrim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r'))
        s[--n] = '\0';
}

static int cfg_set_internal(struct Config *cfg,
                             const char *key, const char *val)
{
    for (int i = 0; i < cfg->count; i++) {
        if (strncmp(cfg->entries[i].key, key, CFG_KEY_MAX) == 0) {
            strncpy(cfg->entries[i].val, val, CFG_VAL_MAX - 1);
            cfg->entries[i].val[CFG_VAL_MAX - 1] = '\0';
            return 0;
        }
    }
    if (cfg->count >= CFG_MAX_ENTRIES) {
        fprintf(stderr,
                "config: warning: entry limit reached, skipping key: %s\n",
                key);
        return -2;
    }
    strncpy(cfg->entries[cfg->count].key, key, CFG_KEY_MAX - 1);
    cfg->entries[cfg->count].key[CFG_KEY_MAX - 1] = '\0';
    strncpy(cfg->entries[cfg->count].val, val, CFG_VAL_MAX - 1);
    cfg->entries[cfg->count].val[CFG_VAL_MAX - 1] = '\0';
    cfg->count++;
    return 0;
}

/* -------------------------------------------------------------------------
 * TOML parser
 * ---------------------------------------------------------------------- */

static void parse_toml(struct Config *cfg, const char *text)
{
    char section[64] = "";
    char line[CFG_LINE_MAX];
    const char *p = text;
    int lineno = 0;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= CFG_LINE_MAX) len = CFG_LINE_MAX - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + len;
        lineno++;

        char *s = ltrim(line);
        rtrim(s);

        if (*s == '\0' || *s == '#') continue;

        if (*s == '[') {
            char *close = strchr(s + 1, ']');
            if (!close) {
                fprintf(stderr,
                        "config: warning: line %d: malformed section header\n",
                        lineno);
                continue;
            }
            size_t slen = (size_t)(close - (s + 1));
            if (slen == 0 || slen >= sizeof(section)) {
                fprintf(stderr,
                        "config: warning: line %d: section name too long or"
                        " empty\n", lineno);
                continue;
            }
            memcpy(section, s + 1, slen);
            section[slen] = '\0';
            char *ts = ltrim(section);
            rtrim(ts);
            memmove(section, ts, strlen(ts) + 1);
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr,
                    "config: warning: line %d: skipping invalid line: %s\n",
                    lineno, s);
            continue;
        }

        *eq = '\0';
        rtrim(s);
        char *key = ltrim(s);
        if (*key == '\0') {
            fprintf(stderr, "config: warning: line %d: empty key\n", lineno);
            continue;
        }

        char *v = ltrim(eq + 1);
        int in_q = 0;
        for (char *vc = v; *vc; vc++) {
            if (*vc == '"')          in_q = !in_q;
            if (*vc == '#' && !in_q) { *vc = '\0'; break; }
        }
        rtrim(v);

        size_t vlen = strlen(v);
        if (vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"') {
            v[vlen - 1] = '\0';
            v++;
        }

        char full_key[CFG_KEY_MAX];
        if (section[0])
            snprintf(full_key, sizeof(full_key), "%s.%s", section, key);
        else
            snprintf(full_key, sizeof(full_key), "%s", key);

        cfg_set_internal(cfg, full_key, v);
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
 * Public API — load
 * ---------------------------------------------------------------------- */

Config *config_load_path(Arena *arena, const char *path)
{
    if (!arena || !path) return NULL;

    struct Config *cfg = arena_alloc(arena, sizeof(struct Config));
    if (!cfg) return NULL;
    memset(cfg, 0, sizeof(struct Config));

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            f = fopen(path, "w");
            if (f) { fputs(s_default_toml_a, f); fputs(s_default_toml_b, f); fclose(f); }
        }
        return cfg;
    }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return cfg; }
    long fsz = ftell(f);
    rewind(f);

    if (fsz <= 0 || fsz > CFG_FILE_MAX) {
        if (fsz > CFG_FILE_MAX)
            fprintf(stderr,
                    "config: warning: file too large (%ld bytes), using"
                    " defaults\n", fsz);
        fclose(f);
        return cfg;
    }

    char *buf = arena_alloc(arena, (size_t)fsz + 1);
    if (!buf) { fclose(f); return cfg; }

    size_t nread = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    buf[nread] = '\0';
    parse_toml(cfg, buf);
    return cfg;
}

Config *config_load(Arena *arena)
{
    if (!arena) return NULL;

    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "config: warning: HOME not set, using empty config\n");
        struct Config *cfg = arena_alloc(arena, sizeof(struct Config));
        if (cfg) memset(cfg, 0, sizeof(struct Config));
        return cfg;
    }

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.nanocode", home);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        fprintf(stderr, "config: warning: could not create %s: %s\n",
                dir, strerror(errno));

    char path[512];
    snprintf(path, sizeof(path), "%s/.nanocode/config.toml", home);
    return config_load_path(arena, path);
}

/* -------------------------------------------------------------------------
 * Public API — getters
 * ---------------------------------------------------------------------- */

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
    char *end;
    long n = strtol(v, &end, 10);
    if (end != v) return n != 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API — config_set
 * ---------------------------------------------------------------------- */

int config_set(Config *cfg, const char *key, const char *val)
{
    if (!cfg || !key) return -1;
    if (!val) val = "";
    return cfg_set_internal(cfg, key, val);
}

/* -------------------------------------------------------------------------
 * Public API — config_save
 * ---------------------------------------------------------------------- */

static const char * const s_sections[] = {
    "provider", "sandbox", "ui", "system_prompt",
    "rendering", "theme", "layout", "behavior", "keys", "performance",
    "session",
    NULL
};

int config_save(const Config *cfg, const char *path)
{
    if (!cfg || !path) { errno = EINVAL; return -1; }

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fputs("# nanocode configuration\n"
          "# ~/.nanocode/config.toml\n"
          "# Generated by :set — edit freely.\n\n", f);

    int written[CFG_MAX_ENTRIES];
    memset(written, 0, sizeof(written));

    for (int si = 0; s_sections[si]; si++) {
        const char *sec = s_sections[si];
        int section_printed = 0;
        size_t slen = strlen(sec);

        for (int di = 0; s_defaults[di].key; di++) {
            if (strncmp(s_defaults[di].key, sec, slen) != 0 ||
                s_defaults[di].key[slen] != '.')
                continue;

            if (!section_printed) {
                fprintf(f, "[%s]\n", sec);
                section_printed = 1;
            }

            const char *short_key = s_defaults[di].key + slen + 1;
            const char *val = config_get_str(cfg, s_defaults[di].key);

            for (int ci = 0; ci < cfg->count; ci++) {
                if (strcmp(cfg->entries[ci].key, s_defaults[di].key) == 0) {
                    written[ci] = 1;
                    break;
                }
            }

            /* Bare (unquoted) for booleans and plain integers. */
            int is_bare = (*val != '\0') &&
                          (strcmp(val, "true") == 0 ||
                           strcmp(val, "false") == 0);
            if (!is_bare && *val != '\0') {
                char *end;
                strtol(val, &end, 10);
                if (*end == '\0') is_bare = 1;
            }

            if (is_bare)
                fprintf(f, "%s = %s\n", short_key, val);
            else
                fprintf(f, "%s = \"%s\"\n", short_key, val);
        }

        if (section_printed) fputc('\n', f);
    }

    /*
     * Extra (user-added) keys: group by the section prefix extracted from
     * the dotted key.  This ensures a correct round-trip through the parser.
     */
    char cur_extra_sec[CFG_KEY_MAX] = "";
    for (int ci = 0; ci < cfg->count; ci++) {
        if (written[ci]) continue;

        const char *full = cfg->entries[ci].key;
        const char *dot  = strchr(full, '.');

        if (dot) {
            char sec[CFG_KEY_MAX];
            size_t slen = (size_t)(dot - full);
            if (slen >= CFG_KEY_MAX) slen = CFG_KEY_MAX - 1;
            memcpy(sec, full, slen);
            sec[slen] = '\0';

            if (strcmp(sec, cur_extra_sec) != 0) {
                fprintf(f, "\n[%s]\n", sec);
                strncpy(cur_extra_sec, sec, CFG_KEY_MAX - 1);
                cur_extra_sec[CFG_KEY_MAX - 1] = '\0';
            }
            fprintf(f, "%s = \"%s\"\n", dot + 1, cfg->entries[ci].val);
        } else {
            if (cur_extra_sec[0]) { fputc('\n', f); cur_extra_sec[0] = '\0'; }
            fprintf(f, "%s = \"%s\"\n", full, cfg->entries[ci].val);
        }
    }

    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API — config_cmd_set
 * ---------------------------------------------------------------------- */

int config_cmd_set(Config *cfg, const char *line)
{
    if (!cfg || !line) return -1;

    char buf[CFG_LINE_MAX];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *s = ltrim(buf);
    if (*s == ':') s++;
    s = ltrim(s);
    if (strncmp(s, "set ", 4) == 0) { s += 4; s = ltrim(s); }

    char *sp = s;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    if (*sp == '\0') return -2;

    *sp = '\0';
    char *key = s;
    char *val = ltrim(sp + 1);
    rtrim(val);

    if (*key == '\0' || *val == '\0') return -2;

    int rc = config_set(cfg, key, val);
    return (rc == 0) ? 0 : -3;
}
