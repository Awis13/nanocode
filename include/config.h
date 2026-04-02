/*
 * config.h -- nanocode configuration system
 *
 * Loads ~/.nanocode/config.toml on startup. Creates the file with compiled-in
 * defaults if it is missing. All storage is arena-allocated; no individual frees.
 *
 * Keys are in "section.name" dotted form, e.g. "provider.api_key".
 *
 * Supported sections and keys:
 *   [provider]      type ("claude"|"openai"|"ollama"), api_key, base_url,
 *                   port (0=default: 443 for claude, 11434 for openai/ollama),
 *                   model, timeout_ms
 *   [sandbox]       enabled (bool), profile
 *   [ui]            theme, word_wrap (bool), stream_delay_ms
 *   [system_prompt] append
 *   [rendering]     target_fps, frame_batch_ms, scroll_mode
 *   [theme]         accent_color, diff_add_color, diff_rm_color,
 *                   syntax_theme, true_color (bool)
 *   [layout]        panel_split, status_bar_position, max_width, padding
 *   [behavior]      auto_approve_tools (bool), confirm_destructive (bool),
 *                   max_context_tokens
 *   [keys]          submit, cancel, scroll_up, scroll_down
 *   [performance]   idle_timeout_ms, max_output_lines, history_limit_mb
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "util/arena.h"

typedef struct Config Config;

Config *config_load_path(Arena *arena, const char *path);
Config *config_load(Arena *arena);

const char *config_get_str(const Config *cfg, const char *key);
int         config_get_int(const Config *cfg, const char *key);
int         config_get_bool(const Config *cfg, const char *key);

int config_set(Config *cfg, const char *key, const char *val);
int config_save(const Config *cfg, const char *path);
int config_cmd_set(Config *cfg, const char *line);

#endif /* CONFIG_H */
