#include <stdio.h>

#include <SDL2/SDL.h>

#include "sdl_info.h"

int sdl_info_main(void)
{
    /*
     * Linked version, not the headers': the launcher is deciding what the
     * device's SDL can do, and the device's SDL is whatever the CFW installed.
     * A header/runtime mismatch is itself worth seeing in a bug report.
     */
    SDL_version linked;
    SDL_GetVersion(&linked);
    const char *revision = SDL_GetRevision();
    printf("sdl: runtime %u.%u.%u (built against %u.%u.%u)%s%s\n",
           linked.major, linked.minor, linked.patch,
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL,
           revision && *revision ? " rev " : "",
           revision && *revision ? revision : "");

    /*
     * The compiled-in list, which is what SDL_VIDEODRIVER may name. It is not
     * the list of backends that would *work* here - a driver can be compiled in
     * and fail to initialise - but a name that is absent can never work, and
     * that is the decision the launcher needs.
     */
    int count = SDL_GetNumVideoDrivers();
    if (count < 0) {
        printf("sdl: video drivers: unavailable (%s)\n", SDL_GetError());
        return 0;
    }

    printf("sdl: video drivers: %d\n", count);
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetVideoDriver(i);
        printf("sdl: video driver: %s\n", name ? name : "(unnamed)");
    }

    return 0;
}
