/*
 * cacerts.h - the root certificates the update check trusts (generated: see tools/gen_cacerts.py)
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_CACERTS_H
#define LS_CACERTS_H

#include <stddef.h>

typedef struct { const unsigned char *der; size_t len; const char *name; } ls_ca_root;
extern const ls_ca_root ls_ca_roots[];
extern const int ls_ca_roots_n;

#endif
