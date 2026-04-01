/*
 * config.h — nanocode configuration system
 *
 * Loads ~/.nanocode/config.toml on startup. Creates the file with compiled-in
 * defaults if it is missing. All storage is arena-allocated; no individual frees.
 *
 * Keys are in "section.name" dotted form, e.g. "provider.api_key".
 *
 * Supported sections and keys:
 *   [provider]      api_key, base_url, model, timeout_ms
 *   [sandbox]       enabled (bool), profile
 *   [ui]            theme, word_wrap (bool), stream_delay_ms
 *   [system_prompt] append
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "../src/util/arena.h"

/* Opaque config handle. */
typedef struct Config Config;

/*
 * Load config from `path`.  If the file does not exist, write defaults to it
 * and return a Config with compiled-in defaults.  Invalid lines are skipped
 * with a warning; the function never crashes.
 *
 * Returns arena-allocated Config, or NULL on OOM.
 */
Config *config_load_path(Arena *arena, const char *path);

/*
 * Load config from ~/.nanocode/config.toml (creating it if absent).
 * Convenience wrapper around config_load_path().
 * Returns NULL on OOM or if HOME is unset.
 */
Config *config_load(Arena *arena);

/*
 * Look up a string value by dotted key (e.g. "provider.model").
 * Returns the compiled-in default string if the key is absent.
 * Never returns NULL.
 */
const char *config_get_str(const Config *cfg, const char *key);

/*
 * Look up an integer value.
 * Returns the compiled-in default (or 0) if the key is absent or non-numeric.
 */
int config_get_int(const Config *cfg, const char *key);

/*
 * Look up a boolean value ("true"/"1" → 1, "false"/"0" → 0).
 * Returns the compiled-in default (or 0) if the key is absent or unrecognised.
 */
int config_get_bool(const Config *cfg, const char *key);

#endif /* CONFIG_H */
