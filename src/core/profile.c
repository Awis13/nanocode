/*
 * profile.c — provider profiles for per-model prompt optimization (CMP-382)
 *
 * Profiles are TOML files that configure model-specific behaviour.
 * User files live in $XDG_CONFIG_HOME/nanocode/profiles/ (default:
 * ~/.config/nanocode/profiles/).  Built-in defaults are compiled in for:
 *   claude   — Anthropic Claude family
 *   gpt      — OpenAI GPT / o-series
 *   ollama   — Ollama local models
 *   default  — fallback for unrecognised models
 *
 * The TOML parser reuses the same simple subset understood by config.c:
 * string / int / float values, sections.  No arrays or inline tables.
 */

#include "../../include/profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * Built-in profile TOML strings
 * ---------------------------------------------------------------------- */

static const char s_claude_toml[] =
    "# Claude profile — Anthropic Claude family\n"
    "# Auto-selected for models with prefix: claude\n"
    "\n"
    "system_prompt_template = \"\"\n"
    "tool_format            = \"native\"\n"
    "context_window         = 200000\n"
    "thinking_budget        = 0\n"
    "temperature            = 1.0\n"
    "top_p                  = 1.0\n"
    "max_output_tokens      = 8192\n"
    "stop_sequences         = \"\"\n";

static const char s_gpt_toml[] =
    "# GPT profile — OpenAI GPT / o-series\n"
    "# Auto-selected for models with prefix: gpt, o1, o3\n"
    "\n"
    "system_prompt_template = \"\"\n"
    "tool_format            = \"function_calling\"\n"
    "context_window         = 128000\n"
    "thinking_budget        = 0\n"
    "temperature            = 1.0\n"
    "top_p                  = 1.0\n"
    "max_output_tokens      = 4096\n"
    "stop_sequences         = \"\"\n";

static const char s_ollama_toml[] =
    "# Ollama profile — local models via Ollama\n"
    "# Auto-selected for models with prefix: ollama, llama, qwen, gemma, mistral\n"
    "\n"
    "system_prompt_template = \"\"\n"
    "tool_format            = \"function_calling\"\n"
    "context_window         = 32768\n"
    "thinking_budget        = 0\n"
    "temperature            = 0.7\n"
    "top_p                  = 0.9\n"
    "max_output_tokens      = 4096\n"
    "stop_sequences         = \"\"\n";

static const char s_default_toml[] =
    "# Default profile — fallback for unrecognised models\n"
    "\n"
    "system_prompt_template = \"\"\n"
    "tool_format            = \"native\"\n"
    "context_window         = 0\n"
    "thinking_budget        = 0\n"
    "temperature            = -1\n"
    "top_p                  = -1\n"
    "max_output_tokens      = 0\n"
    "stop_sequences         = \"\"\n";

/* Map of built-in profile name → TOML source */
static const struct { const char *name; const char *toml; } s_builtins[] = {
    { "claude",  s_claude_toml  },
    { "gpt",     s_gpt_toml     },
    { "ollama",  s_ollama_toml  },
    { "default", s_default_toml },
    { NULL, NULL }
};

/* -------------------------------------------------------------------------
 * Tiny TOML parser — flat only (no sections needed for profile files)
 * ---------------------------------------------------------------------- */

#define PROF_LINE_MAX 512

static char *p_ltrim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void p_rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r'))
        s[--n] = '\0';
}

/* Parse a floating-point value string as integer * 1000 (e.g. "0.7" → 700).
 * Returns -1 if the string represents a negative or absent value (e.g. "-1"). */
static int parse_float_x1000(const char *s)
{
    /* Negative / explicitly unset */
    if (s[0] == '-') return -1;

    double v = strtod(s, NULL);
    if (v < 0.0) return -1;
    return (int)(v * 1000.0 + 0.5);
}

/* arena_strdup: copy s into arena */
static const char *arena_strdup(Arena *arena, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = arena_alloc(arena, n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

static void profile_parse_toml(ProviderProfile *prof, Arena *arena,
                                const char *text)
{
    char line[PROF_LINE_MAX];
    const char *p = text;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= PROF_LINE_MAX) len = PROF_LINE_MAX - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + len;

        char *s = p_ltrim(line);
        p_rtrim(s);
        if (*s == '\0' || *s == '#') continue;
        /* Skip section headers (profiles use flat layout) */
        if (*s == '[') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        p_rtrim(s);
        char *key = p_ltrim(s);
        if (!*key) continue;

        char *v = p_ltrim(eq + 1);
        /* Strip inline comment */
        int in_q = 0;
        for (char *vc = v; *vc; vc++) {
            if (*vc == '"')          in_q = !in_q;
            if (*vc == '#' && !in_q) { *vc = '\0'; break; }
        }
        p_rtrim(v);

        /* Strip surrounding quotes */
        size_t vlen = strlen(v);
        if (vlen >= 2 && v[0] == '"' && v[vlen-1] == '"') {
            v[vlen-1] = '\0';
            v++;
        }

        /* Map key → profile field */
        if (strcmp(key, "system_prompt_template") == 0) {
            prof->system_prompt_template = arena_strdup(arena, v);
        } else if (strcmp(key, "tool_format") == 0) {
            prof->tool_format = arena_strdup(arena, v);
        } else if (strcmp(key, "context_window") == 0) {
            prof->context_window = atoi(v);
        } else if (strcmp(key, "thinking_budget") == 0) {
            prof->thinking_budget = atoi(v);
        } else if (strcmp(key, "temperature") == 0) {
            prof->temperature_x1000 = parse_float_x1000(v);
        } else if (strcmp(key, "top_p") == 0) {
            prof->top_p_x1000 = parse_float_x1000(v);
        } else if (strcmp(key, "max_output_tokens") == 0) {
            prof->max_output_tokens = atoi(v);
        } else if (strcmp(key, "stop_sequences") == 0) {
            if (*v)
                prof->stop_sequences = arena_strdup(arena, v);
        }
    }
}

/* -------------------------------------------------------------------------
 * Profile directory resolution
 * ---------------------------------------------------------------------- */

/* Fill `dir` (capacity `cap`) with the profiles directory path.
 * Respects $XDG_CONFIG_HOME; falls back to ~/.config/nanocode/profiles/. */
static void profile_dir(char *dir, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(dir, cap, "%s/nanocode/profiles", xdg);
    } else {
        const char *home = getenv("HOME");
        snprintf(dir, cap, "%s/.config/nanocode/profiles",
                 home && home[0] ? home : ".");
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

ProviderProfile *profile_load(Arena *arena, const char *name)
{
    if (!arena || !name) name = "default";

    ProviderProfile *prof = arena_alloc(arena, sizeof(ProviderProfile));
    if (!prof) return NULL;
    memset(prof, 0, sizeof(ProviderProfile));
    prof->temperature_x1000 = -1;
    prof->top_p_x1000       = -1;
    prof->name = arena_strdup(arena, name);

    /* 1. Try user file first */
    char dir[512];
    profile_dir(dir, sizeof(dir));
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.toml", dir, name);

    FILE *f = fopen(path, "r");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            rewind(f);
            if (sz > 0 && sz < 65536) {
                char *buf = arena_alloc(arena, (size_t)sz + 1);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)sz, f);
                    buf[n] = '\0';
                    profile_parse_toml(prof, arena, buf);
                }
            }
        }
        fclose(f);
        return prof;
    }

    /* 2. Fall back to built-in */
    for (int i = 0; s_builtins[i].name; i++) {
        if (strcmp(s_builtins[i].name, name) == 0) {
            profile_parse_toml(prof, arena, s_builtins[i].toml);
            return prof;
        }
    }

    /* 3. Unknown name — return empty/sentinel profile */
    return prof;
}

ProviderProfile *profile_for_model(Arena *arena, const char *model)
{
    if (!model || !model[0])
        return profile_load(arena, "default");

    /* Prefix matching table (longest prefix first for specificity) */
    static const struct { const char *prefix; const char *profile; } map[] = {
        { "claude",   "claude"  },
        { "gpt",      "gpt"     },
        { "o1",       "gpt"     },
        { "o3",       "gpt"     },
        { "o4",       "gpt"     },
        { "llama",    "ollama"  },
        { "qwen",     "ollama"  },
        { "gemma",    "ollama"  },
        { "mistral",  "ollama"  },
        { "phi",      "ollama"  },
        { "deepseek", "ollama"  },
        { NULL, NULL }
    };

    for (int i = 0; map[i].prefix; i++) {
        size_t plen = strlen(map[i].prefix);
        if (strncasecmp(model, map[i].prefix, plen) == 0)
            return profile_load(arena, map[i].profile);
    }

    return profile_load(arena, "default");
}

void profile_list(void)
{
    /* Print built-ins */
    printf("Built-in profiles:\n");
    for (int i = 0; s_builtins[i].name; i++)
        printf("  %s\n", s_builtins[i].name);

    /* Scan user directory */
    char dir[512];
    profile_dir(dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d) return;

    int printed_header = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t nlen = strlen(nm);
        if (nlen < 6) continue;                        /* need at least x.toml */
        if (strcmp(nm + nlen - 5, ".toml") != 0) continue;

        /* Check if this shadows a built-in */
        char profile_name[256];
        size_t copy = nlen - 5 < sizeof(profile_name) - 1
                    ? nlen - 5 : sizeof(profile_name) - 1;
        memcpy(profile_name, nm, copy);
        profile_name[copy] = '\0';

        int is_builtin = 0;
        for (int i = 0; s_builtins[i].name; i++) {
            if (strcmp(s_builtins[i].name, profile_name) == 0) {
                is_builtin = 1;
                break;
            }
        }

        if (!printed_header) {
            printf("\nUser profiles (%s):\n", dir);
            printed_header = 1;
        }
        if (is_builtin)
            printf("  %s (overrides built-in)\n", profile_name);
        else
            printf("  %s\n", profile_name);
    }
    closedir(d);
}
