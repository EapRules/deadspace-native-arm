#pragma once

#include <stddef.h>

/*
 * Asset path translation.
 *
 * The engine addresses its data with Android-side names that mean nothing on a
 * Linux filesystem. Every libc entry point that takes a path has to run them
 * through fix_path() first; see src/symtab_io.cpp for the rules and for the
 * evidence they were derived from.
 */

/* Called once from main() with argv[1]. */
void io_set_game_dir(const char *dir);

const char *io_game_dir(void);

/*
 * Translates 'orig' into 'buf' and returns 'buf', or returns 'orig' untouched
 * when no rule applies. The caller owns the buffer, which keeps this usable
 * from inside a libc thunk without allocating - the game calls open() from
 * inside its own allocator's core path, where a malloc would recurse.
 */
const char *fix_path(const char *orig, char *buf, size_t bufsz);
