#pragma once

/*
 * Optional host-control channel for the qemu/Mesa development emulator.
 * Everything is inert unless DEADSPACE_CONTROL_DIR is set.
 */
void emulator_control_init(void);
bool emulator_control_tick(long frame);
void emulator_control_after_draw(long frame, int width, int height);
void emulator_control_shutdown(long frame);
