/*
 * headbin.h - the fake package header the Vita's installer wants (see headbin.c).
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_HEADBIN_H
#define LS_HEADBIN_H

#include <stdint.h>

#define LS_HEAD_BIN_SIZE 1072

/* titleid: "ABCD12345"; contentid may be NULL or "" (then one is made up from the title id). */
void ls_make_head_bin(const char *titleid, const char *contentid, uint8_t out[LS_HEAD_BIN_SIZE]);

#endif
