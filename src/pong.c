/*
 * pong.c - the Pong game (see pong.h). Physics in pixels and seconds; the ball moves in small steps so that it can never jump
 * through a paddle.
 *
 * Author: trashman (VitaSend)
 */
#include "pong.h"
#include "gfx.h"
#include "json.h"
#include "plat.h"
#include "ui.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PW        12                      /* paddle size */
#define PH        76
#define BALL_R    8
#define P_X       (PONG_X + 28)           /* the player's paddle (left edge) */
#define A_X       (PONG_X + PONG_W - 28 - PW)
#define MIN_Y     (PONG_Y)
#define MAX_Y     (PONG_Y + PONG_H)

typedef enum { ST_NONE, ST_READY, ST_PLAY, ST_PAUSED, ST_OVER } pstate;

static struct {
    pstate   st, before_pause;
    float    bx, by, vx, vy;              /* ball */
    float    py, ay;                      /* paddles (top edge) */
    int      score, cpu;
    uint64_t last, ready_until;
    float    ai_target;                   /* where the computer wants the middle of its paddle */
    uint64_t ai_next;
    bool     new_best;
    uint32_t rng;
} G;

/* ------------------------------------------------------------------ the saved part */

static char s_path[300];
static bool s_unlocked;
static int  s_best;

static void save(void)
{
    if (!s_path[0]) return;
    jw w;
    jw_init(&w);
    jw_obj(&w);
    jw_kbool(&w, "unlocked", s_unlocked);
    jw_kint(&w, "best", s_best);
    jw_end_obj(&w);
    if (!w.oom) ls_file_write(s_path, w.buf, w.len);
    jw_free(&w);
}

void pong_load(const char *dir)
{
    ls_path_join(s_path, sizeof s_path, dir, "pong.json");
    s_unlocked = false;
    s_best = 0;
    uint8_t *data = NULL;
    size_t len = 0;
    if (!ls_file_read(s_path, &data, &len)) return;
    char err[80];
    json_val *j = json_parse((const char *)data, len, err, sizeof err);
    if (j && j->type == J_OBJ) {
        s_unlocked = json_bool(json_get(j, "unlocked"), false);
        double b = json_num(json_get(j, "best"), 0);
        s_best = (b >= 0 && b < 1000000) ? (int)b : 0;
    }
    json_free(j);
    free(data);
}

bool pong_unlocked(void) { return s_unlocked; }
void pong_unlock(void) { if (!s_unlocked) { s_unlocked = true; save(); } }
int  pong_best(void) { return s_best; }

/* ------------------------------------------------------------------ the game */

static float frand(void)                  /* 0 .. 1 */
{
    G.rng ^= G.rng << 13;
    G.rng ^= G.rng >> 17;
    G.rng ^= G.rng << 5;
    return (float)(G.rng & 0xFFFFu) / 65535.0f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

#ifdef LS_HOST
/* test knobs (PC build only): LS_PONG_TIMESCALE = game speed factor, LS_PONG_LIVES = points the computer needs, LS_PONG_CPU_SPEED = its paddle speed */
static float knob(const char *name, float dflt)
{
    const char *e = getenv(name);
    return e && *e ? (float)atof(e) : dflt;
}
#define TIMESCALE() knob("LS_PONG_TIMESCALE", 1.0f)
#define LIVES()     ((int)knob("LS_PONG_LIVES", (float)PONG_LIVES))
#define CPU_SPEED() knob("LS_PONG_CPU_SPEED", -1.0f)
#else
#define TIMESCALE() 1.0f
#define LIVES()     PONG_LIVES
#define CPU_SPEED() (-1.0f)
#endif

static float speed_now(void) { return sqrtf(G.vx * G.vx + G.vy * G.vy); }

static void center_ball(void)
{
    G.bx = PONG_X + PONG_W / 2.0f;
    G.by = PONG_Y + PONG_H / 2.0f;
    G.vx = G.vy = 0;
}

static void start_ready(uint64_t now, int toward)            /* toward: +1 = the ball will go to the computer, -1 = to you */
{
    center_ball();
    G.st = ST_READY;
    G.ready_until = now + 1100;
    G.vx = (float)toward;                                    /* (only the direction, until the serve) */
}

static void serve(void)
{
    float dir = G.vx >= 0 ? 1.0f : -1.0f;
    float speed = 300.0f + (float)(G.score < 10 ? G.score : 10) * 8.0f;
    float ang = (frand() - 0.5f) * 0.8f;                     /* +-23 degrees */
    G.vx = dir * speed * cosf(ang);
    G.vy = speed * sinf(ang);
    G.st = ST_PLAY;
}

void pong_new_game(void)
{
    if (!G.rng) G.rng = (uint32_t)plat_time_ms() | 1u;
    G.score = G.cpu = 0;
    G.py = G.ay = PONG_Y + (PONG_H - PH) / 2.0f;
    G.new_best = false;
    G.ai_target = PONG_Y + PONG_H / 2.0f;
    G.ai_next = 0;
    G.last = plat_time_ms();
    start_ready(G.last, +1);
}

bool pong_started(void) { return G.st != ST_NONE; }
bool pong_over(void) { return G.st == ST_OVER; }
bool pong_paused(void) { return G.st == ST_PAUSED; }
int  pong_score(void) { return G.score; }

void pong_pause(bool on)
{
    if (on && (G.st == ST_READY || G.st == ST_PLAY)) { G.before_pause = G.st; G.st = ST_PAUSED; }
    else if (!on && G.st == ST_PAUSED) {
        G.st = G.before_pause;
        G.last = plat_time_ms();                             /* no jump for the time spent paused */
        if (G.st == ST_READY) G.ready_until = G.last + 800;
    }
}

void pong_finish(void)
{
    if (G.st != ST_NONE && G.score > s_best) { s_best = G.score; save(); }
}

/* Where the ball will be when it reaches x (walls fold the path back). */
static float predict_y(float x)
{
    if (G.vx <= 1.0f) return PONG_Y + PONG_H / 2.0f;
    float t = (x - G.bx) / G.vx;
    float lo = MIN_Y + BALL_R, hi = MAX_Y - BALL_R, span = hi - lo;
    float y = G.by + G.vy * t - lo;
    float m = fmodf(y, 2 * span);
    if (m < 0) m += 2 * span;
    if (m > span) m = 2 * span - m;
    return lo + m;
}

static void ai_think(uint64_t now)
{
    if (now < G.ai_next) return;
    G.ai_next = now + 110;                                   /* reaction time: it looks at the ball ~9 times a second */
    if (G.vx > 0) {
        float err = (frand() - 0.5f) * 2.0f * 30.0f;         /* it never aims perfectly */
        G.ai_target = predict_y((float)A_X) + err;
    } else {
        G.ai_target = PONG_Y + PONG_H / 2.0f;                /* back to the middle while the ball is away */
    }
}

static void paddle_hit(bool player)
{
    float py = player ? G.py : G.ay;
    float rel = clampf((G.by - (py + PH / 2.0f)) / (PH / 2.0f), -1.0f, 1.0f);
    float ang = rel * 0.95f;                                  /* up to ~54 degrees */
    float speed = clampf(speed_now() * 1.06f, 300.0f, 720.0f);
    float dir = player ? 1.0f : -1.0f;
    G.vx = dir * speed * cosf(ang);
    G.vy = speed * sinf(ang);
    G.bx = player ? (float)(P_X + PW) + BALL_R : (float)A_X - BALL_R;
}

static void point_for(bool player, uint64_t now)
{
    if (player) G.score++;
    else G.cpu++;
    if (G.cpu >= LIVES()) {
        G.st = ST_OVER;
        center_ball();
        if (G.score > s_best) { s_best = G.score; G.new_best = true; save(); }
        return;
    }
    start_ready(now, player ? +1 : -1);                       /* the one who missed gets the ball */
}

void pong_step(uint64_t now, const pong_input *in)
{
    if (G.st == ST_NONE) return;
    float dt = (float)(now - G.last) / 1000.0f;
    G.last = now;
    if (dt <= 0) return;
    if (dt > 0.05f) dt = 0.05f;
    dt *= TIMESCALE();
    if (G.st == ST_PAUSED || G.st == ST_OVER) return;

    /* your paddle: D-pad, or the finger (it follows quickly, but not instantly) */
    float step = 360.0f * dt;
    if (in->up) G.py -= step;
    if (in->down) G.py += step;
    if (in->touch) {
        float want = (float)in->touch_y - PH / 2.0f, d = want - G.py, m = 1100.0f * dt;
        G.py += clampf(d, -m, m);
    }
    G.py = clampf(G.py, MIN_Y, MAX_Y - PH);

    if (G.st == ST_READY) {
        float mid = PONG_Y + (PONG_H - PH) / 2.0f, d = mid - G.ay, m = 200.0f * dt;
        G.ay += clampf(d, -m, m);
        if (now >= G.ready_until) serve();
        return;
    }

    /* the computer's paddle: limited speed, a little quicker with every point you make */
    ai_think(now);
    float ai_speed = 230.0f + 14.0f * (float)(G.score < 16 ? G.score : 16);
    if (CPU_SPEED() >= 0) ai_speed = CPU_SPEED();
    float d = (G.ai_target - PH / 2.0f) - G.ay, m = ai_speed * dt;
    G.ay = clampf(G.ay + clampf(d, -m, m), MIN_Y, MAX_Y - PH);

    /* the ball, in steps of at most 4 pixels */
    float dist = speed_now() * dt;
    int n = (int)(dist / 4.0f) + 1;
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        G.bx += G.vx * h;
        G.by += G.vy * h;
        if (G.by < MIN_Y + BALL_R) { G.by = MIN_Y + BALL_R; G.vy = fabsf(G.vy); }
        if (G.by > MAX_Y - BALL_R) { G.by = MAX_Y - BALL_R; G.vy = -fabsf(G.vy); }
        if (G.vx < 0 && G.bx - BALL_R <= P_X + PW && G.bx > P_X && G.by > G.py - BALL_R && G.by < G.py + PH + BALL_R) paddle_hit(true);
        else if (G.vx > 0 && G.bx + BALL_R >= A_X && G.bx < A_X + PW && G.by > G.ay - BALL_R && G.by < G.ay + PH + BALL_R) paddle_hit(false);
        if (G.bx < PONG_X - 24) { point_for(false, now); return; }
        if (G.bx > PONG_X + PONG_W + 24) { point_for(true, now); return; }
    }
}

/* ------------------------------------------------------------------ drawing */

static void overlay(const char *title, const char *l1, const char *l2)
{
    gfx_rrect(PONG_X, PONG_Y, PONG_W, PONG_H, 14, GRGBA(0, 0, 0, 150));
    int cx = PONG_X + PONG_W / 2, cy = PONG_Y + PONG_H / 2;
    gfx_text(cx, cy - 34, 34, GW_BOLD, GRGB(255, 255, 255), GA_CENTER, title);
    if (l1) gfx_text(cx, cy + 12, 17, GW_MED, C_ACCENT_HI, GA_CENTER, l1);
    if (l2) gfx_text(cx, cy + 42, 15, GW_REG, GRGB(200, 205, 212), GA_CENTER, l2);
}

void pong_draw(void)
{
    gfx_rrect(PONG_X, PONG_Y, PONG_W, PONG_H, 14, C_CARD);
    gfx_rrect_outline(PONG_X, PONG_Y, PONG_W, PONG_H, 14, 2, C_BORDER);
    for (int y = PONG_Y + 12; y < PONG_Y + PONG_H - 14; y += 26)                         /* the net */
        gfx_rect(PONG_X + PONG_W / 2 - 2, y, 4, 14, C_BORDER);

    if (G.st != ST_NONE) {
        gfx_rrect(P_X, (int)(G.py + 0.5f), PW, PH, 5, C_ACCENT_HI);
        gfx_rrect(A_X, (int)(G.ay + 0.5f), PW, PH, 5, C_DIM);
        if (G.st != ST_OVER) gfx_circle(G.bx, G.by, BALL_R, GRGB(255, 255, 255));
    }

    /* the score, between the logo and the battery */
    char a[8], b[8];
    snprintf(a, sizeof a, "%d", G.score);
    snprintf(b, sizeof b, "%d", G.cpu);
    int mx = GFX_W / 2;
    gfx_text(mx - 34, 38, 38, GW_BOLD, C_TEXT, GA_RIGHT, a);
    gfx_text(mx, 40, 34, GW_BOLD, C_FAINT, GA_CENTER, ":");
    gfx_text(mx + 34, 38, 38, GW_BOLD, C_TEXT, GA_LEFT, b);
    gfx_text(mx - 34, 64, 11, GW_MED, C_ACCENT_HI, GA_RIGHT, "YOU");
    char lives[24];
    snprintf(lives, sizeof lives, "CPU  (%d left)", LIVES() - G.cpu);
    gfx_text(mx + 34, 64, 11, GW_MED, C_DIM, GA_LEFT, lives);

    /* the best score, bottom right (while you beat it, the live score shows) */
    char best[40];
    int shown = G.score > s_best ? G.score : s_best;
    snprintf(best, sizeof best, "Best  %d", shown);
    gfx_text(CONTENT_R, 512, 16, GW_BOLD, G.score > s_best ? C_GOOD : C_DIM, GA_RIGHT, best);

    switch (G.st) {
    case ST_READY:  gfx_text(PONG_X + PONG_W / 2, PONG_Y + PONG_H / 2 + 70, 15, GW_REG, C_FAINT, GA_CENTER, "Get ready"); break;
    case ST_PAUSED: overlay("Paused", "START  Resume", "O  Exit"); break;
    case ST_OVER: {
        char l1[64];
        snprintf(l1, sizeof l1, "%d : %d%s", G.score, G.cpu, G.new_best ? "   -   New best!" : "");
        overlay("Game over", l1, "X  New game      O  Exit");
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ host only: simulated games
 *
 * `LS_PONG_SIM=<games>[,<seed>]` (PC test build): plays whole games with a bot instead of a person - fixed 16 ms steps, no window -
 * prints how many rallies the bot won, and ends the program. The bot plays like a decent human: D-pad only (the paddle moves at the
 * D-pad speed), it reacts after a short delay, aims a little off, and sends the ball to the side the computer is not on by hitting
 * with the edge of the paddle. It is how the test checks that the computer can be beaten (and is not a pushover either).
 */
#ifdef LS_HOST
static float bot_predict(void)                               /* where the ball will be when it reaches the player's paddle */
{
    if (G.vx >= -1.0f) return PONG_Y + PONG_H / 2.0f;
    float t = ((float)(P_X + PW) + BALL_R - G.bx) / G.vx;
    float lo = MIN_Y + BALL_R, hi = MAX_Y - BALL_R, span = hi - lo;
    float y = G.by + G.vy * t - lo;
    float m = fmodf(y, 2 * span);
    if (m < 0) m += 2 * span;
    if (m > span) m = 2 * span - m;
    return lo + m;
}

void pong_simulate(const char *spec)
{
    int games = atoi(spec);
    const char *comma = strchr(spec, ',');
    uint32_t seed = comma ? (uint32_t)atoi(comma + 1) : 1u;
    const char *comma2 = comma ? strchr(comma + 1, ',') : NULL;
    float noise = comma2 ? (float)atof(comma2 + 1) : 14.0f;           /* how far beside the ball the bot's paddle may end up (px) */
    if (games < 1) games = 1;
    s_path[0] = 0;                                           /* never touch the saved best */
    long total = 0, cpu_total = 0;
    int best = 0, worst = 1 << 30, five = 0;
    for (int g = 0; g < games; g++) {
        G.rng = 0;
        pong_new_game();
        G.rng = (seed * 2654435761u + (uint32_t)g * 40503u) | 1u;
        uint64_t now = G.last;
        uint64_t bot_next = 0;
        float want = PONG_Y + PONG_H / 2.0f, bias = 0;
        bool was_away = true;
        for (long step = 0; step < 60L * 60 * 20 && !pong_over(); step++) {       /* at most 20 minutes of play */
            now += 16;
            if (G.vx < 0 && was_away) {                      /* the ball just turned towards the bot: a new guess and a new reaction time */
                bot_next = now + 170;
                bias = (frand() - 0.5f) * 2.0f * noise;
                was_away = false;
            }
            if (G.vx >= 0) was_away = true;
            if (now >= bot_next) {
                bot_next = now + 100;
                if (G.vx < 0) {
                    float cpu_mid = G.ay + PH / 2.0f;
                    float aim = cpu_mid < PONG_Y + PONG_H / 2.0f ? 0.6f : -0.6f;      /* hit with the edge: away from the computer */
                    want = bot_predict() - aim * (PH / 2.0f) + bias;
                } else {
                    want = PONG_Y + PONG_H / 2.0f;
                }
            }
            pong_input in = {0};
            float mid = G.py + PH / 2.0f;
            if (noise >= 0) {                                /* (a negative noise = nobody plays: the paddle never moves) */
                if (want < mid - 4.0f) in.up = true;
                else if (want > mid + 4.0f) in.down = true;
            }
            pong_step(now, &in);
        }
        total += G.score;
        cpu_total += G.cpu;
        if (G.score > best) best = G.score;
        if (G.score < worst) worst = G.score;
        if (G.score >= 5) five++;
    }
    printf("PONG_SIM games=%d bot_rallies=%ld cpu_rallies=%ld avg=%.2f best=%d worst=%d games_with_5=%d\n",
           games, total, cpu_total, (double)total / games, best, worst, five);
    fflush(stdout);
    exit(0);
}
#endif
