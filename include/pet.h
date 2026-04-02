/*
 * pet.h — terminal pet: sprite data model and animation state machine
 *
 * The pet is a small ASCII-art companion rendered alongside the TUI.
 * It animates through states that mirror the coding session lifecycle:
 *
 *   IDLE   ──(auto, PET_SLEEP_TICKS)──► SLEEP
 *   IDLE   ──(external transition)────► ACTIVE
 *   ACTIVE ──(external transition)────► DONE | ERROR
 *   any    ──(external transition)────► ERROR
 *
 * No heap allocations — all state is stack / static storage.
 */

#ifndef PET_H
#define PET_H

#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Enums
 * ---------------------------------------------------------------------- */

typedef enum {
    PET_CAT  = 0,
    PET_CRAB = 1,
    PET_DOG  = 2,
    PET_OFF  = 3   /* disabled / no pet */
} PetKind;

typedef enum {
    PET_IDLE   = 0,
    PET_ACTIVE = 1,
    PET_DONE   = 2,
    PET_ERROR  = 3,
    PET_SLEEP  = 4
} PetState;

/* Ticks before IDLE auto-transitions to SLEEP (~30 s at 100 ms tick rate). */
#define PET_SLEEP_TICKS 300

/* -------------------------------------------------------------------------
 * Struct
 * ---------------------------------------------------------------------- */

typedef struct {
    PetKind  kind;
    PetState state;
    int      frame_index;
    int      idle_ticks;  /* counts render ticks since last state change */
} Pet;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/*
 * Initialise with IDLE state and frame 0.
 */
Pet pet_new(PetKind kind);

/*
 * Change state, reset frame_index and idle_ticks.
 */
void pet_transition(Pet *p, PetState new_state);

/*
 * Advance frame_index (wrapping), increment idle_ticks.
 * Auto-transitions IDLE → SLEEP after PET_SLEEP_TICKS ticks.
 */
void pet_tick(Pet *p);

/*
 * Return the current animation frame string (~8 chars wide × 4 lines,
 * newline-separated). Never returns NULL.
 * use_color: include ANSI colour escapes when true.
 */
const char *pet_frame(const Pet *p, bool use_color);

/*
 * Parse "cat" / "crab" / "dog" / "off" → PetKind.
 * Falls back to PET_CAT and logs a warning on unknown input.
 */
PetKind pet_kind_from_str(const char *name);

/*
 * Number of selectable pet kinds (PET_CAT, PET_CRAB, PET_DOG — excludes
 * PET_OFF).
 */
int pet_kind_count(void);

/*
 * Pick a random selectable pet kind.
 */
PetKind pet_kind_random(void);

#endif /* PET_H */
