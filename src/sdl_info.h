/*
 * What SDL is on this device, asked of SDL rather than of the firmware's name.
 *
 * A Knulli (Batocera-derived) user got this port, Mass Effect and Real Racing 3
 * running by hand-exporting SDL_VIDEODRIVER="mali" before the launch line. That
 * backend is not in upstream SDL: it is a vendor patch some firmwares carry and
 * others do not, and asking for it where it does not exist makes SDL_Init fail
 * on a device that was working. So the launcher has to know whether this SDL
 * has it, and only SDL can answer.
 *
 * SDL_GetNumVideoDrivers/SDL_GetVideoDriver read the compiled-in bootstrap
 * table and need no SDL_Init - which matters, because the point is to decide
 * what to set *before* the game initialises SDL, and because SDL_Init on a
 * console with the wrong backend is the failure being avoided.
 *
 * Which SDL does this report? The one the game will use: libSDL2 is
 * deliberately not bundled (tools/collect_libs.sh leaves it to the device,
 * every CFW builds its own against its own video backend), so this binary and
 * the game run against the same system libSDL2-2.0.so.0. Inside the build
 * container that is Debian's SDL, whose driver list has x11/wayland/offscreen
 * and no mali - correct for the container, and nothing like a console's.
 */
#ifndef DEADSPACE_SDL_INFO_H
#define DEADSPACE_SDL_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prints, on stdout, one "sdl: video driver: <name>" line per compiled-in
 * video driver plus a version line. Returns a process exit status.
 */
int sdl_info_main(void);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPACE_SDL_INFO_H */
