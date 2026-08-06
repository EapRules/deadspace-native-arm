#include <SDL2/SDL.h>

#include "app_exit.h"
#include "trace.h"

/*
 * Two mechanisms for one request, because neither alone is sufficient.
 *
 * SDL_PushEvent is the thread-safe half: it takes SDL's event lock, so a
 * finish() raised on an engine thread still reaches the loop. The flag is the
 * half that does not depend on the queue - SDL_PushEvent returns 0 when the
 * queue is full or SDL_QUIT has been disabled, and an exit request that is
 * dropped is exactly the freeze this file exists to remove.
 *
 * The flag is sig_atomic_t rather than a plain int so the store is indivisible
 * even if the request ever arrives from a signal handler.
 */
static volatile sig_atomic_t g_exit_requested = 0;

void android_app_request_exit(const char *reason)
{
    if (g_exit_requested)
        return;
    g_exit_requested = 1;

    trace("exit requested by the game (%s) - stopping the frame loop",
          reason ? reason : "no reason given");

    SDL_Event quit = {};
    quit.type = SDL_QUIT;
    if (SDL_PushEvent(&quit) < 0)
        trace("exit: SDL_PushEvent(SDL_QUIT) refused (%s); the loop's own "
              "check still stops it", SDL_GetError());
}

bool android_app_exit_requested(void)
{
    return g_exit_requested != 0;
}
