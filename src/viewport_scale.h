#pragma once

/*
 * The engine's fixed logical panel mapped onto the device's real drawable.
 * Implemented in symtab_glprobe.cpp, next to the glViewport/glScissor probes
 * that apply the same transform to the engine's own calls. Anything the port
 * paints in physical pixels - the software cursor - has to go through here or
 * it stays confined to the logical rectangle.
 */
extern "C" {

/* Called once, from main.cpp, with the drawable and the engine's logical size. */
void viewport_scale_init(int phys_w, int phys_h, int log_w, int log_h);

/*
 * Map a point from logical game space to physical panel pixels. Both spaces
 * have their origin at the top left, which is the convention the engine's
 * touch coordinates already use. Identity when the panel matches the logical
 * size, so callers never need to ask whether scaling is on.
 */
void viewport_scale_map(float *x, float *y);

/*
 * Whole-number magnification for port-drawn sprites, so they keep their pixel
 * art crisp instead of being resampled. Always at least 1.
 */
int viewport_scale_factor(void);

}
