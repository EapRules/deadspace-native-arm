#pragma once

/*
 * Optional per-call GL error attribution.
 *
 * DEADSPACE_GL_DIAG=1 makes the dynamic GLES tables route every entry through
 * their typed ABI bridge. The bridge asks these hooks to drain the error queue
 * immediately before and after the real driver call, so an error is reported
 * against the function that actually generated it.
 */
bool gl_diag_enabled(void);
void gl_diag_before(const char *name);
void gl_diag_after(const char *name);
