/*
 * history.c - The list of the last 40 transfers and text messages (history.json on the memory card).
 *
 * Author: trashman (VitaSend)
 */
#include "history.h"
#include "json.h"
#include "plat.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hist_entry  s_list[HIST_MAX];     /* oldest first */
static int         s_n;
static plat_mutex *s_lock;
static char        s_path[300];

static void save_locked(void)
{
    if (!s_path[0]) return;
    jw w;
    jw_init(&w);
    jw_arr(&w);
    for (int i = 0; i < s_n; i++) {
        const hist_entry *e = &s_list[i];
        jw_obj(&w);
        jw_kint(&w, "time", e->time);
        jw_kbool(&w, "out", e->outgoing);
        jw_kint(&w, "status", e->status);
        jw_kbool(&w, "isText", e->is_text);
        jw_kstr(&w, "peer", e->peer);
        jw_kint(&w, "files", e->nfiles);
        jw_kint(&w, "bytes", (int64_t)e->bytes);
        jw_kstr(&w, "summary", e->summary);
        if (e->is_text) jw_kstr(&w, "text", e->text);
        jw_end_obj(&w);
    }
    jw_end_arr(&w);
    if (!w.oom) {
        char tmp[320];
        snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
        if (ls_file_write(tmp, w.buf, w.len)) {
            plat_remove(s_path);
            plat_rename(tmp, s_path);
        }
    }
    jw_free(&w);
}

void history_load(const char *dir)
{
    if (!s_lock) s_lock = plat_mutex_create();
    plat_mutex_lock(s_lock);
    s_n = 0;
    ls_path_join(s_path, sizeof s_path, dir, "history.json");
    uint8_t *data = NULL;
    size_t len = 0;
    if (ls_file_read(s_path, &data, &len)) {
        char err[80];
        json_val *j = json_parse((const char *)data, len, err, sizeof err);
        if (j && j->type == J_ARR) {
            size_t from = j->n > HIST_MAX ? j->n - HIST_MAX : 0;
            for (size_t i = from; i < j->n; i++) {
                const json_val *o = j->items[i];
                hist_entry *e = &s_list[s_n];
                memset(e, 0, sizeof *e);
                e->time = (int64_t)json_num(json_get(o, "time"), 0);
                e->outgoing = json_bool(json_get(o, "out"), false);
                double st = json_num(json_get(o, "status"), 0);
                e->status = (st >= 0 && st <= 3) ? (hist_status)(int)st : H_FAILED;
                e->is_text = json_bool(json_get(o, "isText"), false);
                ls_strlcpy(e->peer, json_str(json_get(o, "peer"), ""), sizeof e->peer);
                e->nfiles = (int)json_num(json_get(o, "files"), 0);
                e->bytes = (uint64_t)json_num(json_get(o, "bytes"), 0);
                ls_strlcpy(e->summary, json_str(json_get(o, "summary"), ""), sizeof e->summary);
                ls_strlcpy(e->text, json_str(json_get(o, "text"), ""), sizeof e->text);
                s_n++;
            }
        }
        json_free(j);
        free(data);
    }
    plat_mutex_unlock(s_lock);
}

void history_add(const hist_entry *e)
{
    if (!s_lock) return;
    plat_mutex_lock(s_lock);
    if (s_n == HIST_MAX) {
        memmove(&s_list[0], &s_list[1], (HIST_MAX - 1) * sizeof s_list[0]);
        s_n--;
    }
    s_list[s_n++] = *e;
    save_locked();
    plat_mutex_unlock(s_lock);
}

int history_count(void)
{
    if (!s_lock) return 0;
    plat_mutex_lock(s_lock);
    int n = s_n;
    plat_mutex_unlock(s_lock);
    return n;
}

bool history_get(int idx, hist_entry *out)
{
    if (!s_lock) return false;
    bool ok = false;
    plat_mutex_lock(s_lock);
    if (idx >= 0 && idx < s_n) {
        *out = s_list[s_n - 1 - idx];
        ok = true;
    }
    plat_mutex_unlock(s_lock);
    return ok;
}
