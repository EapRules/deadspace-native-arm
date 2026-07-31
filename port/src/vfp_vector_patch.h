#pragma once

struct so_module;

/*
 * Expand the game's obsolete VFP short-vector arithmetic into scalar A32
 * trampolines. Returns the number of validated instructions patched.
 */
unsigned int patch_vfp_short_vectors(so_module *mod);
