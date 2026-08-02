/*
 * Framebuffer probe.
 *
 * "The game presented 120 frames" and "the game presented 120 black frames"
 * look exactly the same in a log, and the second one is what a port full of
 * well-behaved stubs produces. The only way to tell them apart without a human
 * looking at a screen is to read the pixels back.
 */
#ifndef DEADSPACE_FB_PROBE_H
#define DEADSPACE_FB_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reads the default framebuffer back and reports whether anything was drawn.
 *
 * Must be called with the GL context current and BEFORE the swap: after it the
 * back buffer is undefined. `frame` is the 1-based frame number, used to space
 * the probes out.
 */
void android_fb_probe(long frame, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPACE_FB_PROBE_H */
