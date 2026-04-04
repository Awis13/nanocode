/*
 * profile.h — provider profiles for per-model prompt optimization (CMP-382)
 *
 * Provider profiles are TOML files that tune model-specific behaviour:
 * system prompt template, tool format, context window, sampling params, etc.
 *
 * Profile resolution order (highest → lowest priority):
 *   1. User file  ~/.config/nanocode/profiles/<name>.toml
 *      (XDG_CONFIG_HOME is respected: $XDG_CONFIG_HOME/nanocode/profiles/)
 *   2. Built-in compiled-in defaults for "claude", "gpt", "ollama", "default"
 *
 * Auto-selection by model name prefix:
 *   "claude*"                → "claude" profile
 *   "gpt*" | "o1*" | "o3*"  → "gpt"    profile
 *   "ollama*" | (local host) → "ollama" profile
 *   (anything else)          → "default" profile
 *
 * Supported TOML fields (all optional):
 *   system_prompt_template  = "..."   # appended after the base system prompt
 *   tool_format             = "native" | "function_calling"
 *   context_window          = 200000  # max input tokens
 *   thinking_budget         = 10000   # extended reasoning budget (Claude)
 *   temperature             = 1.0     # sampling temperature (0.0–2.0)
 *   top_p                   = 0.9     # nucleus sampling threshold
 *   max_output_tokens       = 4096    # cap on response length
 *   stop_sequences          = "###,END"  # comma-separated stop strings
 */

#ifndef PROFILE_H
#define PROFILE_H

#include "util/arena.h"

/*
 * ProviderProfile — loaded representation of a single profile.
 *
 * Unset integer fields use sentinel 0 (or -1 for temperature/top_p where 0
 * is a valid value).  Unset string fields are NULL.
 * All string pointers are arena-allocated; no separate frees.
 */
typedef struct {
    const char *name;                  /* profile name, e.g. "claude"        */
    const char *system_prompt_template;/* appended to base system prompt     */
    const char *tool_format;           /* "native" | "function_calling"      */
    int         context_window;        /* 0 = use provider/model default     */
    int         thinking_budget;       /* 0 = disabled                       */
    int         temperature_x1000;     /* temp * 1000; -1 = unset            */
    int         top_p_x1000;           /* top_p * 1000; -1 = unset           */
    int         max_output_tokens;     /* 0 = use provider default           */
    const char *stop_sequences;        /* comma-separated; NULL = none       */
} ProviderProfile;

/*
 * profile_load — load a profile by name.
 *
 * Searches user directory first, then falls back to built-in defaults.
 * Returns an arena-allocated ProviderProfile (never NULL — unrecognised names
 * yield an empty/default profile).
 */
ProviderProfile *profile_load(Arena *arena, const char *name);

/*
 * profile_for_model — auto-select a profile based on model name prefix.
 *
 * Uses prefix matching to pick the closest built-in profile, then loads it
 * via profile_load().  Falls back to "default" if no prefix matches.
 * Returns an arena-allocated ProviderProfile (never NULL).
 */
ProviderProfile *profile_for_model(Arena *arena, const char *model);

/*
 * profile_list — print available profiles to stdout.
 *
 * Lists built-in profiles followed by any user files found in the profiles
 * directory.  Format: one name per line, with a "(user)" suffix for user
 * files that shadow or supplement a built-in.
 */
void profile_list(void);

#endif /* PROFILE_H */
