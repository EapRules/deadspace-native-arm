#include <dlfcn.h>
#include <stdio.h>

#include "gl_probe.h"

int gl_probe_main(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: deadspace --gl-probe <library> [symbol ...]\n");
        return 2;
    }

    const char *path = argv[0];

    /*
     * RTLD_NOW | RTLD_LOCAL is what SDL_LoadObject() uses, and SDL is the
     * consumer whose failure this preflight is standing in for. Loading it any
     * other way would answer a question nobody asked: a blob that resolves
     * lazily here and faults inside SDL_CreateWindow is exactly the outcome
     * being prevented.
     */
    dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *error = dlerror();
        printf("%s\n", error ? error : "dlopen failed without a reason");
        return GL_PROBE_EXIT_UNUSABLE;
    }

    for (int i = 1; i < argc; i++) {
        dlerror();
        void *symbol = dlsym(handle, argv[i]);
        if (!symbol) {
            const char *error = dlerror();
            printf("%s: %s\n", argv[i], error ? error : "symbol not found");
            dlclose(handle);
            return GL_PROBE_EXIT_UNUSABLE;
        }
    }

    dlclose(handle);
    return 0;
}
