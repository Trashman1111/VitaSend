/*
 * pong.h - the easter egg: a small game of Pong (you on the left, the computer on the right).
 *
 * Self-contained: it only uses gfx.h / ui.h (drawing and theme colours), plat.h (time, files) and json.h. Everything that connects
 * it to the rest of the program (the logo taps that unlock it, L + R, the screen, the top bar) is in app_pong.c.
 *
 * Rules: you score when the computer misses; the match is over when the computer has scored PONG_LIVES points. The ball gets faster
 * with every return and the computer gets a little quicker with every point you make. The best score is kept in <data>/pong.json
 * together with the "unlocked" flag, so nothing of this touches config.json.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_PONG_H
#define LS_PONG_H

#include <stdbool.h>
#include <stdint.h>

#define PONG_LIVES 5

/* the court (screen pixels): below the top bar, above the bottom row */
#define PONG_X 20
#define PONG_Y 80
#define PONG_W 920
#define PONG_H 400

typedef struct {
    bool up, down;           /* D-pad held                                              */
    bool touch;              /* a finger is on the screen ...                           */
    int  touch_y;            /* ... at this height (screen pixels): the paddle follows it */
} pong_input;

/* ---- the saved part ---- */
void pong_load(const char *dir);        /* <dir>/pong.json; a missing file = locked, best 0 */
bool pong_unlocked(void);
void pong_unlock(void);                 /* saves */
int  pong_best(void);

/* ---- the game ---- */
void pong_new_game(void);               /* a new match (starts with a short "ready" pause) */
bool pong_started(void);                /* a match exists (running, paused or over) */
bool pong_over(void);
bool pong_paused(void);
void pong_pause(bool on);               /* no effect once the match is over */
void pong_finish(void);                 /* leaving the game: a better score than the saved one is saved now */
int  pong_score(void);                  /* your points in this match */

void pong_step(uint64_t now_ms, const pong_input *in);       /* call often (every frame) */
void pong_draw(void);                   /* the court, the score, the overlays and "Best N" at the bottom right; the caller draws the rest */

#ifdef LS_HOST
void pong_simulate(const char *spec);          /* PC test build: plays whole games with a bot, prints the result and exits (LS_PONG_SIM) */
#endif

#endif
