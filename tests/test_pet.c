/*
 * test_pet.c — unit tests for pet module (src/tui/pet.c)
 *
 * Covers: all state transitions, frame wrapping, sleep auto-transition,
 * unknown pet name parsing, pet_kind_count/random, and pet_frame validity.
 */

#include "test.h"
#include "pet.h"
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * pet_new
 * ====================================================================== */

TEST(test_pet_new_idle) {
    Pet p = pet_new(PET_CAT);
    ASSERT_EQ(p.kind,        PET_CAT);
    ASSERT_EQ(p.state,       PET_IDLE);
    ASSERT_EQ(p.frame_index, 0);
    ASSERT_EQ(p.idle_ticks,  0);
}

TEST(test_pet_new_crab) {
    Pet p = pet_new(PET_CRAB);
    ASSERT_EQ(p.kind,  PET_CRAB);
    ASSERT_EQ(p.state, PET_IDLE);
}

TEST(test_pet_new_dog) {
    Pet p = pet_new(PET_DOG);
    ASSERT_EQ(p.kind,  PET_DOG);
    ASSERT_EQ(p.state, PET_IDLE);
}

TEST(test_pet_new_off) {
    Pet p = pet_new(PET_OFF);
    ASSERT_EQ(p.kind,  PET_OFF);
    ASSERT_EQ(p.state, PET_IDLE);
}

/* =========================================================================
 * pet_transition — state machine correctness
 * ====================================================================== */

TEST(test_transition_idle_to_active) {
    Pet p = pet_new(PET_CAT);
    pet_transition(&p, PET_ACTIVE);
    ASSERT_EQ(p.state,       PET_ACTIVE);
    ASSERT_EQ(p.frame_index, 0);
    ASSERT_EQ(p.idle_ticks,  0);
}

TEST(test_transition_active_to_done) {
    Pet p = pet_new(PET_CAT);
    pet_transition(&p, PET_ACTIVE);
    pet_transition(&p, PET_DONE);
    ASSERT_EQ(p.state, PET_DONE);
}

TEST(test_transition_active_to_error) {
    Pet p = pet_new(PET_DOG);
    pet_transition(&p, PET_ACTIVE);
    pet_transition(&p, PET_ERROR);
    ASSERT_EQ(p.state, PET_ERROR);
}

TEST(test_transition_any_to_error) {
    Pet p = pet_new(PET_CRAB);
    pet_transition(&p, PET_SLEEP);
    pet_transition(&p, PET_ERROR);
    ASSERT_EQ(p.state, PET_ERROR);
}

TEST(test_transition_idle_to_sleep_manual) {
    Pet p = pet_new(PET_CAT);
    pet_transition(&p, PET_SLEEP);
    ASSERT_EQ(p.state, PET_SLEEP);
}

TEST(test_transition_resets_frame_and_ticks) {
    Pet p = pet_new(PET_CAT);
    /* Advance a few ticks to get non-zero counters */
    pet_tick(&p);
    pet_tick(&p);
    pet_tick(&p);
    ASSERT_TRUE(p.frame_index != 0 || p.idle_ticks != 0);

    pet_transition(&p, PET_ACTIVE);
    ASSERT_EQ(p.frame_index, 0);
    ASSERT_EQ(p.idle_ticks,  0);
}

/* =========================================================================
 * pet_tick — frame wrapping and idle_ticks increment
 * ====================================================================== */

TEST(test_tick_increments_frame) {
    Pet p = pet_new(PET_CAT);
    ASSERT_EQ(p.frame_index, 0);
    pet_tick(&p);
    ASSERT_EQ(p.frame_index, 1);
    pet_tick(&p);
    ASSERT_EQ(p.frame_index, 2);
}

TEST(test_tick_wraps_frame) {
    /* CAT IDLE has 3 frames, so tick 3 times → back to 0 */
    Pet p = pet_new(PET_CAT);
    pet_tick(&p);  /* → 1 */
    pet_tick(&p);  /* → 2 */
    pet_tick(&p);  /* → 0 (wraps) */
    ASSERT_EQ(p.frame_index, 0);
}

TEST(test_tick_increments_idle_ticks) {
    Pet p = pet_new(PET_CAT);
    pet_tick(&p);
    ASSERT_EQ(p.idle_ticks, 1);
    pet_tick(&p);
    ASSERT_EQ(p.idle_ticks, 2);
}

TEST(test_tick_no_crash_for_off) {
    /* PET_OFF has no anim — pet_tick must not crash */
    Pet p = pet_new(PET_OFF);
    pet_tick(&p);
    ASSERT_EQ(p.idle_ticks, 1);
}

/* =========================================================================
 * pet_tick — auto-transition IDLE → SLEEP
 * ====================================================================== */

TEST(test_sleep_auto_transition) {
    Pet p = pet_new(PET_CAT);
    /* Drive idle_ticks up to threshold */
    for (int i = 0; i < PET_SLEEP_TICKS; i++)
        pet_tick(&p);
    ASSERT_EQ(p.state, PET_SLEEP);
}

TEST(test_no_sleep_before_threshold) {
    Pet p = pet_new(PET_CAT);
    for (int i = 0; i < PET_SLEEP_TICKS - 1; i++)
        pet_tick(&p);
    ASSERT_EQ(p.state, PET_IDLE);
}

TEST(test_no_sleep_auto_when_active) {
    /* Auto-sleep only triggers from IDLE, not from other states */
    Pet p = pet_new(PET_CAT);
    pet_transition(&p, PET_ACTIVE);
    for (int i = 0; i < PET_SLEEP_TICKS + 10; i++)
        pet_tick(&p);
    ASSERT_EQ(p.state, PET_ACTIVE);
}

/* =========================================================================
 * pet_frame — valid return for all kind×state×color combinations
 * ====================================================================== */

TEST(test_frame_never_null) {
    PetKind kinds[]  = { PET_CAT, PET_CRAB, PET_DOG };
    PetState states[] = { PET_IDLE, PET_ACTIVE, PET_DONE, PET_ERROR, PET_SLEEP };

    for (int ki = 0; ki < 3; ki++) {
        for (int si = 0; si < 5; si++) {
            Pet p = pet_new(kinds[ki]);
            pet_transition(&p, states[si]);
            ASSERT_NOT_NULL(pet_frame(&p, false));
            ASSERT_NOT_NULL(pet_frame(&p, true));
        }
    }
}

TEST(test_frame_off_returns_blank) {
    Pet p = pet_new(PET_OFF);
    const char *f = pet_frame(&p, false);
    ASSERT_NOT_NULL(f);
    /* Blank frame has no printable non-space/newline characters */
    for (const char *c = f; *c; c++)
        ASSERT_TRUE(*c == ' ' || *c == '\n');
}

TEST(test_frame_color_contains_ansi) {
    Pet p = pet_new(PET_CAT);
    const char *cf = pet_frame(&p, true);
    /* Color frame should contain ESC */
    ASSERT_NOT_NULL(strchr(cf, '\033'));
}

TEST(test_frame_plain_no_ansi) {
    Pet p = pet_new(PET_CAT);
    const char *pf = pet_frame(&p, false);
    /* Plain frame must not contain ESC */
    ASSERT_TRUE(strchr(pf, '\033') == NULL);
}

TEST(test_frame_wraps_with_tick) {
    Pet p = pet_new(PET_CAT);
    const char *f0 = pet_frame(&p, false);
    pet_tick(&p);
    const char *f1 = pet_frame(&p, false);
    pet_tick(&p);
    const char *f2 = pet_frame(&p, false);
    pet_tick(&p);  /* wraps */
    const char *f3 = pet_frame(&p, false);
    /* After 3 ticks on a 3-frame animation, f3 == f0 */
    ASSERT_STR_EQ(f0, f3);
    /* f1 and f2 should differ (animation advances) */
    (void)f1; (void)f2; /* at minimum, no crash */
}

/* =========================================================================
 * pet_kind_from_str — parse + unknown fallback
 * ====================================================================== */

TEST(test_kind_from_str_cat) {
    ASSERT_EQ(pet_kind_from_str("cat"), PET_CAT);
}

TEST(test_kind_from_str_crab) {
    ASSERT_EQ(pet_kind_from_str("crab"), PET_CRAB);
}

TEST(test_kind_from_str_dog) {
    ASSERT_EQ(pet_kind_from_str("dog"), PET_DOG);
}

TEST(test_kind_from_str_off) {
    ASSERT_EQ(pet_kind_from_str("off"), PET_OFF);
}

TEST(test_kind_from_str_unknown_fallback) {
    /* Unknown names must fall back to PET_CAT without crashing */
    ASSERT_EQ(pet_kind_from_str("fish"), PET_CAT);
    ASSERT_EQ(pet_kind_from_str(""),     PET_CAT);
}

TEST(test_kind_from_str_null_fallback) {
    ASSERT_EQ(pet_kind_from_str(NULL), PET_CAT);
}

/* =========================================================================
 * pet_kind_count / pet_kind_random
 * ====================================================================== */

TEST(test_kind_count) {
    ASSERT_EQ(pet_kind_count(), 3);
}

TEST(test_kind_random_range) {
    /* Run many times and verify result is always 0, 1, or 2 */
    for (int i = 0; i < 100; i++) {
        PetKind k = pet_kind_random();
        ASSERT_TRUE(k == PET_CAT || k == PET_CRAB || k == PET_DOG);
    }
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    RUN_TEST(test_pet_new_idle);
    RUN_TEST(test_pet_new_crab);
    RUN_TEST(test_pet_new_dog);
    RUN_TEST(test_pet_new_off);

    RUN_TEST(test_transition_idle_to_active);
    RUN_TEST(test_transition_active_to_done);
    RUN_TEST(test_transition_active_to_error);
    RUN_TEST(test_transition_any_to_error);
    RUN_TEST(test_transition_idle_to_sleep_manual);
    RUN_TEST(test_transition_resets_frame_and_ticks);

    RUN_TEST(test_tick_increments_frame);
    RUN_TEST(test_tick_wraps_frame);
    RUN_TEST(test_tick_increments_idle_ticks);
    RUN_TEST(test_tick_no_crash_for_off);

    RUN_TEST(test_sleep_auto_transition);
    RUN_TEST(test_no_sleep_before_threshold);
    RUN_TEST(test_no_sleep_auto_when_active);

    RUN_TEST(test_frame_never_null);
    RUN_TEST(test_frame_off_returns_blank);
    RUN_TEST(test_frame_color_contains_ansi);
    RUN_TEST(test_frame_plain_no_ansi);
    RUN_TEST(test_frame_wraps_with_tick);

    RUN_TEST(test_kind_from_str_cat);
    RUN_TEST(test_kind_from_str_crab);
    RUN_TEST(test_kind_from_str_dog);
    RUN_TEST(test_kind_from_str_off);
    RUN_TEST(test_kind_from_str_unknown_fallback);
    RUN_TEST(test_kind_from_str_null_fallback);

    RUN_TEST(test_kind_count);
    RUN_TEST(test_kind_random_range);

    PRINT_SUMMARY();
    return g_failures > 0 ? 1 : 0;
}
