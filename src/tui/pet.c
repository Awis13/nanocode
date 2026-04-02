/*
 * pet.c — terminal pet: sprite data, state machine, animation
 */

#include "pet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * ANSI colour helpers
 * ====================================================================== */

#define A_YLW  "\033[33m"   /* yellow  — cat  */
#define A_RED  "\033[31m"   /* red     — crab */
#define A_CYN  "\033[36m"   /* cyan    — dog  */
#define A_RST  "\033[0m"

/* =========================================================================
 * Sprite data — each frame is 4 lines of ~8 visible chars, '\n'-separated.
 * Plain variants contain only printable ASCII; colour variants wrap them
 * in ANSI escape codes.
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * CAT sprites
 * ---------------------------------------------------------------------- */

/* --- IDLE (sitting, blink, tail-flick) --- */
static const char *const s_cat_idle_p[] = {
    " /\\_/\\  \n (^.^)  \n (> <)  \n        ",
    " /\\_/\\  \n (-.-)  \n (> <)  \n        ",
    " /\\_/\\ ~\n (^.^)  \n (> <)  \n        ",
};
static const char *const s_cat_idle_c[] = {
    A_YLW " /\\_/\\  \n (^.^)  \n (> <)  \n        " A_RST,
    A_YLW " /\\_/\\  \n (-.-)  \n (> <)  \n        " A_RST,
    A_YLW " /\\_/\\ ~\n (^.^)  \n (> <)  \n        " A_RST,
};

/* --- ACTIVE (leaping) --- */
static const char *const s_cat_active_p[] = {
    " /\\_/\\  \n(>^.^>) \n  ><    \n  ||    ",
    " /\\_/\\  \n(>^.^>) \n  ||    \n /  \\  ",
    " /\\_/\\  \n(>^.^>) \n  ><    \n  ||    ",
};
static const char *const s_cat_active_c[] = {
    A_YLW " /\\_/\\  \n(>^.^>) \n  ><    \n  ||    " A_RST,
    A_YLW " /\\_/\\  \n(>^.^>) \n  ||    \n /  \\  " A_RST,
    A_YLW " /\\_/\\  \n(>^.^>) \n  ><    \n  ||    " A_RST,
};

/* --- DONE (content, sparkles) --- */
static const char *const s_cat_done_p[] = {
    " /\\_/\\* \n(=^.^=) \n  (__)  \n *    * ",
    " /\\_/\\  \n(=^.^=)*\n  (__)  \n*      *",
    "*/\\_/\\  \n(=^.^=) \n  (__) *\n        ",
};
static const char *const s_cat_done_c[] = {
    A_YLW " /\\_/\\* \n(=^.^=) \n  (__)  \n *    * " A_RST,
    A_YLW " /\\_/\\  \n(=^.^=)*\n  (__)  \n*      *" A_RST,
    A_YLW "*/\\_/\\  \n(=^.^=) \n  (__) *\n        " A_RST,
};

/* --- ERROR (X eyes, confusion) --- */
static const char *const s_cat_error_p[] = {
    " /\\_/\\  \n(=x.x=) \n  ???   \n  !!!   ",
    " /\\_/\\  \n(=X.X=) \n  ???   \n  !!!   ",
    " /\\_/\\  \n(=x.x=) \n  ???   \n  !!!   ",
};
static const char *const s_cat_error_c[] = {
    A_YLW " /\\_/\\  \n(=x.x=) \n  ???   \n  !!!   " A_RST,
    A_YLW " /\\_/\\  \n(=X.X=) \n  ???   \n  !!!   " A_RST,
    A_YLW " /\\_/\\  \n(=x.x=) \n  ???   \n  !!!   " A_RST,
};

/* --- SLEEP (zzz drifting right) --- */
static const char *const s_cat_sleep_p[] = {
    " /\\_/\\  \n(=-.-)  \n z      \n        ",
    " /\\_/\\  \n(=-.-)  \n  zZ    \n        ",
    " /\\_/\\  \n(=-.-)  \n   zZZ  \n        ",
};
static const char *const s_cat_sleep_c[] = {
    A_YLW " /\\_/\\  \n(=-.-)  \n z      \n        " A_RST,
    A_YLW " /\\_/\\  \n(=-.-)  \n  zZ    \n        " A_RST,
    A_YLW " /\\_/\\  \n(=-.-)  \n   zZZ  \n        " A_RST,
};

/* -------------------------------------------------------------------------
 * CRAB sprites
 * ---------------------------------------------------------------------- */

/* --- IDLE (claws at sides, blink, claw-raise) --- */
static const char *const s_crab_idle_p[] = {
    "  ___   \n (o.o)  \n/|   |\\ \n |___|  ",
    "  ___   \n (-.-)  \n/|   |\\ \n |___|  ",
    "\\ ___ /\n (o.o)  \n |   |  \n |___|  ",
};
static const char *const s_crab_idle_c[] = {
    A_RED "  ___   \n (o.o)  \n/|   |\\ \n |___|  " A_RST,
    A_RED "  ___   \n (-.-)  \n/|   |\\ \n |___|  " A_RST,
    A_RED "\\ ___ /\n (o.o)  \n |   |  \n |___|  " A_RST,
};

/* --- ACTIVE (claws raised, scuttling) --- */
static const char *const s_crab_active_p[] = {
    "\\(___)/ \n (o.o)  \n |   |  \n |___|  ",
    " \\___/  \n(>o.o<) \n |   |  \n |___|  ",
    "\\(___)/ \n (o.o)  \n |   |  \n |___|  ",
};
static const char *const s_crab_active_c[] = {
    A_RED "\\(___)/ \n (o.o)  \n |   |  \n |___|  " A_RST,
    A_RED " \\___/  \n(>o.o<) \n |   |  \n |___|  " A_RST,
    A_RED "\\(___)/ \n (o.o)  \n |   |  \n |___|  " A_RST,
};

/* --- DONE (sparkle on shell) --- */
static const char *const s_crab_done_p[] = {
    "  _*_   \n (^.^)  \n/|   |\\ \n |___|  ",
    "  ___  *\n (^.^)  \n/|   |\\ \n |___|  ",
    "* ___ *\n (^.^)  \n/|   |\\ \n |___|  ",
};
static const char *const s_crab_done_c[] = {
    A_RED "  _*_   \n (^.^)  \n/|   |\\ \n |___|  " A_RST,
    A_RED "  ___  *\n (^.^)  \n/|   |\\ \n |___|  " A_RST,
    A_RED "* ___ *\n (^.^)  \n/|   |\\ \n |___|  " A_RST,
};

/* --- ERROR (X eyes, claws shaking) --- */
static const char *const s_crab_error_p[] = {
    "  ___   \n (x.x)  \n/|   |\\ \n |___|  ",
    "  ___   \n (X.X)  \n!!   !! \n |___|  ",
    "  ___   \n (x.x)  \n/|   |\\ \n |___|  ",
};
static const char *const s_crab_error_c[] = {
    A_RED "  ___   \n (x.x)  \n/|   |\\ \n |___|  " A_RST,
    A_RED "  ___   \n (X.X)  \n!!   !! \n |___|  " A_RST,
    A_RED "  ___   \n (x.x)  \n/|   |\\ \n |___|  " A_RST,
};

/* --- SLEEP (zzz rising) --- */
static const char *const s_crab_sleep_p[] = {
    "  ___   \n (-.-) z\n/|   |\\ \n |___|  ",
    "  ___   \n (-.-)zZ\n/|   |\\ \n |___|  ",
    "  ___  Z\n (-.-)  \n/|   |\\ \n |___|  ",
};
static const char *const s_crab_sleep_c[] = {
    A_RED "  ___   \n (-.-) z\n/|   |\\ \n |___|  " A_RST,
    A_RED "  ___   \n (-.-)zZ\n/|   |\\ \n |___|  " A_RST,
    A_RED "  ___  Z\n (-.-)  \n/|   |\\ \n |___|  " A_RST,
};

/* -------------------------------------------------------------------------
 * DOG sprites
 * ---------------------------------------------------------------------- */

/* --- IDLE (sitting, tail-wag, blink) --- */
static const char *const s_dog_idle_p[] = {
    "  __    \n (oo)   \n  ||    \n /  \\  ",
    "  __    \n (oo) ~ \n  ||    \n /  \\  ",
    "  __    \n (--)   \n  ||    \n /  \\  ",
};
static const char *const s_dog_idle_c[] = {
    A_CYN "  __    \n (oo)   \n  ||    \n /  \\  " A_RST,
    A_CYN "  __    \n (oo) ~ \n  ||    \n /  \\  " A_RST,
    A_CYN "  __    \n (--)   \n  ||    \n /  \\  " A_RST,
};

/* --- ACTIVE (running) --- */
static const char *const s_dog_active_p[] = {
    "  __    \n (oo)>  \n  )(    \n /\\ /\\ ",
    "  __    \n (oo)>  \n  ||    \n /\\ /\\ ",
    "  __    \n (oo)>  \n  )(    \n /\\ /\\ ",
};
static const char *const s_dog_active_c[] = {
    A_CYN "  __    \n (oo)>  \n  )(    \n /\\ /\\ " A_RST,
    A_CYN "  __    \n (oo)>  \n  ||    \n /\\ /\\ " A_RST,
    A_CYN "  __    \n (oo)>  \n  )(    \n /\\ /\\ " A_RST,
};

/* --- DONE (happy, sparkles) --- */
static const char *const s_dog_done_p[] = {
    "  __  * \n (^^)   \n  ||    \n /  \\  ",
    " *__*   \n (^^)   \n  ||    \n /  \\  ",
    "  __    \n*(^^) * \n  ||    \n /  \\  ",
};
static const char *const s_dog_done_c[] = {
    A_CYN "  __  * \n (^^)   \n  ||    \n /  \\  " A_RST,
    A_CYN " *__*   \n (^^)   \n  ||    \n /  \\  " A_RST,
    A_CYN "  __    \n*(^^) * \n  ||    \n /  \\  " A_RST,
};

/* --- ERROR (confused) --- */
static const char *const s_dog_error_p[] = {
    "  __    \n (><)   \n  ||    \n /  \\  ",
    "  __    \n (><) ! \n  !!    \n /  \\  ",
    "  __    \n (><)   \n  ||    \n /  \\  ",
};
static const char *const s_dog_error_c[] = {
    A_CYN "  __    \n (><)   \n  ||    \n /  \\  " A_RST,
    A_CYN "  __    \n (><) ! \n  !!    \n /  \\  " A_RST,
    A_CYN "  __    \n (><)   \n  ||    \n /  \\  " A_RST,
};

/* --- SLEEP (zzz) --- */
static const char *const s_dog_sleep_p[] = {
    "  __    \n (--)   \n  || z  \n /  \\  ",
    "  __    \n (--)   \n  ||  z \n /  \\  ",
    "  __    \n (--)  z\n  ||    \n /  \\  ",
};
static const char *const s_dog_sleep_c[] = {
    A_CYN "  __    \n (--)   \n  || z  \n /  \\  " A_RST,
    A_CYN "  __    \n (--)   \n  ||  z \n /  \\  " A_RST,
    A_CYN "  __    \n (--)  z\n  ||    \n /  \\  " A_RST,
};

/* =========================================================================
 * Animation table
 * ====================================================================== */

typedef struct {
    int                   count;
    const char *const    *plain;
    const char *const    *color;
} Anim;

/*
 * s_anims[kind][state]  (kind: 0=CAT, 1=CRAB, 2=DOG; state: 0-4)
 */
static const Anim s_anims[3][5] = {
    /* PET_CAT */
    {
        { 3, s_cat_idle_p,   s_cat_idle_c   },  /* PET_IDLE   */
        { 3, s_cat_active_p, s_cat_active_c },  /* PET_ACTIVE */
        { 3, s_cat_done_p,   s_cat_done_c   },  /* PET_DONE   */
        { 3, s_cat_error_p,  s_cat_error_c  },  /* PET_ERROR  */
        { 3, s_cat_sleep_p,  s_cat_sleep_c  },  /* PET_SLEEP  */
    },
    /* PET_CRAB */
    {
        { 3, s_crab_idle_p,   s_crab_idle_c   },
        { 3, s_crab_active_p, s_crab_active_c },
        { 3, s_crab_done_p,   s_crab_done_c   },
        { 3, s_crab_error_p,  s_crab_error_c  },
        { 3, s_crab_sleep_p,  s_crab_sleep_c  },
    },
    /* PET_DOG */
    {
        { 3, s_dog_idle_p,   s_dog_idle_c   },
        { 3, s_dog_active_p, s_dog_active_c },
        { 3, s_dog_done_p,   s_dog_done_c   },
        { 3, s_dog_error_p,  s_dog_error_c  },
        { 3, s_dog_sleep_p,  s_dog_sleep_c  },
    },
};

/* Blank frame returned for PET_OFF or out-of-range states. */
static const char s_blank_frame[] =
    "        \n        \n        \n        ";

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

static const Anim *anim_for(const Pet *p)
{
    if ((int)p->kind >= 3 || (int)p->state > 4)
        return NULL;
    return &s_anims[p->kind][p->state];
}

/* =========================================================================
 * Public API
 * ====================================================================== */

Pet pet_new(PetKind kind)
{
    Pet p;
    memset(&p, 0, sizeof(p));
    p.kind        = kind;
    p.state       = PET_IDLE;
    p.frame_index = 0;
    p.idle_ticks  = 0;
    return p;
}

void pet_transition(Pet *p, PetState new_state)
{
    p->state       = new_state;
    p->frame_index = 0;
    p->idle_ticks  = 0;
}

void pet_tick(Pet *p)
{
    /* Advance animation frame (wrapping). */
    const Anim *a = anim_for(p);
    if (a)
        p->frame_index = (p->frame_index + 1) % a->count;

    /* Increment idle-tick counter. */
    p->idle_ticks++;

    /* Auto-transition IDLE → SLEEP after threshold. */
    if (p->state == PET_IDLE && p->idle_ticks >= PET_SLEEP_TICKS)
        pet_transition(p, PET_SLEEP);
}

const char *pet_frame(const Pet *p, bool use_color)
{
    const Anim *a = anim_for(p);
    if (!a)
        return s_blank_frame;

    int fi = p->frame_index % a->count;
    return use_color ? a->color[fi] : a->plain[fi];
}

PetKind pet_kind_from_str(const char *name)
{
    if (!name)
        goto unknown;

    if (strcmp(name, "cat")  == 0) return PET_CAT;
    if (strcmp(name, "crab") == 0) return PET_CRAB;
    if (strcmp(name, "dog")  == 0) return PET_DOG;
    if (strcmp(name, "off")  == 0) return PET_OFF;

unknown:
    fprintf(stderr, "[pet] unknown pet name \"%s\", defaulting to cat\n",
            name ? name : "(null)");
    return PET_CAT;
}

int pet_kind_count(void)
{
    return 3;  /* PET_CAT, PET_CRAB, PET_DOG */
}

PetKind pet_kind_random(void)
{
    return (PetKind)(rand() % 3);
}
