/*
 * The one translation unit that holds Wuffs' code. See third_party/wuffs-config/inkwell_wuffs.h
 * for what is in it and what deliberately is not; the two decoders built on it are
 * src/codec/png.c and src/codec/inflate.c.
 */

#define WUFFS_IMPLEMENTATION
#include "inkwell_wuffs.h"
