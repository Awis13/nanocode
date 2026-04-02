/*
 * sandbox.h -- nanocode OS-level sandbox interface (CMP-198)
 *
 * Config-driven sandboxing for autonomous server deployment.
 * Backend: sandbox-exec SBPL (macOS) / Landlock LSM (Linux).
 *
 * Typical call sequence:
 *   1. config_load()
 *   2. sandbox_config_from_cfg(&sc, cfg)
 *   3. sandbox_validate(&sc)           -- exit(1) on hard errors
 *   4. sandbox_activate(&sc)           -- apply OS policy, early in main()
 *   5. (system prompt builder calls sandbox_build_prompt_block to inform model)
 */

#ifndef SANDBOX_H
#define SANDBOX_H

#include "config.h"
#include <stddef.h>

/* -------------------------------------------------------------------------
 * SandboxConfig -- parsed sandbox parameters
 * ---------------------------------------------------------------------- */

typedef struct {
    int         enabled;           /* 1 if sandbox mode is active             */
    const char *profile;           /* "strict" | "permissive" | "custom"      */
    const char *allowed_paths;     /* colon-separated absolute paths, or ""   */
    const char *allowed_commands;  /* colon-separated command basenames, or "" */
    int         network;           /* 1 = outbound network permitted           */
    long        max_file_size;     /* max bytes per file write (0 = unlimited) */
} SandboxConfig;

/* -------------------------------------------------------------------------
 * sandbox_config_from_cfg -- populate SandboxConfig from a loaded Config
 *
 * Reads sandbox.* keys from cfg.  Pointers in sc point into cfg's arena;
 * do not free individually.
 * ---------------------------------------------------------------------- */
void sandbox_config_from_cfg(SandboxConfig *sc, const Config *cfg);

/* -------------------------------------------------------------------------
 * sandbox_validate -- validate config before activation
 *
 * Returns  0  on success.
 * Returns -1  on invalid config (prints reason to stderr).
 *
 * Checks:
 *   - profile is one of: strict, permissive, custom
 *   - if profile == "custom": allowed_paths and allowed_commands must be
 *     non-empty
 *   - if profile == "strict": OS enforcement must be available
 *     (#ifdef __APPLE__ or #ifdef __linux__); prints error and returns -1
 *     if neither backend is compiled in
 * ---------------------------------------------------------------------- */
int sandbox_validate(const SandboxConfig *sc);

/* -------------------------------------------------------------------------
 * sandbox_activate -- apply OS-level enforcement policy
 *
 * Must be called once, early in main(), after sandbox_validate() passes.
 * After this call the process is confined by the kernel; the call is
 * not reversible.
 *
 * macOS:  builds an SBPL deny-all policy string and calls sandbox_init(3).
 * Linux:  creates a Landlock ruleset, adds path rules, then calls
 *         landlock_restrict_self(2) after PR_SET_NO_NEW_PRIVS.
 *
 * Returns  0  on success.
 * Returns -1  if OS enforcement is unavailable and profile is "strict"
 *             (logs reason to stderr).
 * Returns  0  (with a warning) if profile is "permissive" and enforcement
 *             fails -- process continues unconfined.
 * ---------------------------------------------------------------------- */
int sandbox_activate(const SandboxConfig *sc);

/* -------------------------------------------------------------------------
 * sandbox_build_prompt_block -- generate <sandbox_policy> system prompt block
 *
 * Writes a NUL-terminated XML-tagged block into buf[0..bufsz-1].
 * The block informs the model of the active sandbox constraints so it
 * can cooperate and explain restrictions to the user instead of silently
 * failing.
 *
 * Does nothing (buf[0] = '\0') when sc->enabled == 0.
 * Safe to call before sandbox_activate(); does not require activation.
 *
 * bufsz should be >= 512 bytes.  Output is truncated to bufsz-1 if needed.
 * ---------------------------------------------------------------------- */
void sandbox_build_prompt_block(const SandboxConfig *sc,
                                char *buf, size_t bufsz);

#endif /* SANDBOX_H */
