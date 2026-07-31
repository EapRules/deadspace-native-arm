#pragma once

/*
 * Draw the touch-only menu cursor over the default framebuffer. The game is
 * GLES 1.1, so this deliberately uses fixed-function-safe state only.
 */
extern "C" void android_cursor_draw(int fb_width, int fb_height);
