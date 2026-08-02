#ifndef DEADSPACE_PATCH_H
#define DEADSPACE_PATCH_H

#include "so_util.h"

/*
 * Rewrite the three instructions in the game's I/O layer that pick between
 * reading assets through JNI and reading them with plain file descriptors.
 *
 * Must run after so_relocate_all() and before so_initialize(): the module is
 * still writable at that point (so_load_module() calls so_flush_caches(mod, 1)
 * just before the so_after_relocate() hook) and none of the game's own code
 * has executed yet.
 */
void so_patch_binary(so_module *mod);

#endif
