#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gl_probe.h"

/* --------------------------------------------------------------- reporting */

static void emitf(gl_probe_report_fn emit, void *ctx, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void emitf(gl_probe_report_fn emit, void *ctx, const char *fmt, ...)
{
    if (!emit)
        return;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emit(ctx, line);
}

void gl_probe_report_stdout(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
}

/* ------------------------------------------------------ tier 1: loadability */

int gl_probe_load(const char *path, const char *const *symbols, int symbol_count,
                  char *error, size_t error_size)
{
    if (error && error_size)
        error[0] = '\0';

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
        const char *reason = dlerror();
        if (error && error_size)
            snprintf(error, error_size, "%s",
                     reason ? reason : "dlopen failed without a reason");
        return GL_PROBE_EXIT_UNUSABLE;
    }

    for (int i = 0; i < symbol_count; i++) {
        dlerror();
        void *symbol = dlsym(handle, symbols[i]);
        if (!symbol) {
            const char *reason = dlerror();
            if (error && error_size)
                snprintf(error, error_size, "%s: %s", symbols[i],
                         reason ? reason : "symbol not found");
            dlclose(handle);
            return GL_PROBE_EXIT_UNUSABLE;
        }
    }

    dlclose(handle);
    return 0;
}

/* ----------------------------------------------------- tier 2: EGL bring-up */

/*
 * EGL's own types, spelled out rather than included.
 *
 * The build image has no EGL headers and must not need any: this file dlopens
 * whatever the device ships and calls four functions through pointers. Taking
 * a build dependency on a header, to describe a library that is deliberately
 * not linked, would be backwards.
 */
typedef void *EGLDisplay;
typedef void *EGLNativeDisplayType;
typedef int EGLint;
typedef unsigned int EGLBoolean;

#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_VENDOR          0x3053
#define EGL_VERSION_STRING  0x3054
#define EGL_CLIENT_APIS     0x308D

using EglGetDisplayFn  = EGLDisplay (*)(EGLNativeDisplayType);
using EglInitializeFn  = EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *);
using EglTerminateFn   = EGLBoolean (*)(EGLDisplay);
using EglQueryStringFn = const char *(*)(EGLDisplay, EGLint);
using EglGetErrorFn    = EGLint (*)(void);

/*
 * A bare 0x3009 in a user's log is a number nobody looks up. The name is what
 * lets the log answer the question without a second round trip.
 */
static const char *egl_error_name(EGLint code)
{
    switch (code) {
    case 0x3000: return "EGL_SUCCESS";
    case 0x3001: return "EGL_NOT_INITIALIZED";
    case 0x3002: return "EGL_BAD_ACCESS";
    case 0x3003: return "EGL_BAD_ALLOC";
    case 0x3004: return "EGL_BAD_ATTRIBUTE";
    case 0x3005: return "EGL_BAD_CONFIG";
    case 0x3006: return "EGL_BAD_CONTEXT";
    case 0x3007: return "EGL_BAD_CURRENT_SURFACE";
    case 0x3008: return "EGL_BAD_DISPLAY";
    case 0x3009: return "EGL_BAD_MATCH";
    case 0x300A: return "EGL_BAD_NATIVE_PIXMAP";
    case 0x300B: return "EGL_BAD_NATIVE_WINDOW";
    case 0x300C: return "EGL_BAD_PARAMETER";
    case 0x300D: return "EGL_BAD_SURFACE";
    case 0x300E: return "EGL_CONTEXT_LOST";
    default:     return "unknown EGL error";
    }
}

int gl_probe_init(const char *path, gl_probe_report_fn emit, void *ctx)
{
    dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *reason = dlerror();
        emitf(emit, ctx, "egl: dlopen %s FAILED - %s", path,
              reason ? reason : "no reason given");
        return GL_PROBE_EXIT_UNUSABLE;
    }
    emitf(emit, ctx, "egl: dlopen %s ok", path);

    /*
     * Every symbol is looked up before any is called, and a missing one is
     * reported rather than fatal: "eglInitialize is not exported at all" and
     * "eglInitialize returned false" are different diagnoses, and a probe that
     * stopped at the first would hide the second behind it.
     */
    struct {
        const char *name;
        void       *fn;
    } symbols[] = {
        { "eglGetDisplay",  NULL },
        { "eglInitialize",  NULL },
        { "eglQueryString", NULL },
        { "eglTerminate",   NULL },
        { "eglGetError",    NULL },
    };
    const size_t symbol_count = sizeof(symbols) / sizeof(symbols[0]);

    int missing = 0;
    for (size_t i = 0; i < symbol_count; i++) {
        dlerror();
        symbols[i].fn = dlsym(handle, symbols[i].name);
        if (!symbols[i].fn) {
            const char *reason = dlerror();
            emitf(emit, ctx, "egl: symbol %s MISSING - %s", symbols[i].name,
                  reason ? reason : "not found");
            missing++;
        }
    }
    if (!missing)
        emitf(emit, ctx, "egl: all %zu entry points resolved", symbol_count);

    EglGetDisplayFn  get_display  = (EglGetDisplayFn)symbols[0].fn;
    EglInitializeFn  initialize   = (EglInitializeFn)symbols[1].fn;
    EglQueryStringFn query_string = (EglQueryStringFn)symbols[2].fn;
    EglTerminateFn   terminate    = (EglTerminateFn)symbols[3].fn;
    EglGetErrorFn    get_error    = (EglGetErrorFn)symbols[4].fn;

    /* Without eglGetError the codes are unavailable, not the whole probe. */
    auto report_error = [&](const char *step) {
        if (!get_error) {
            emitf(emit, ctx, "egl: %s - eglGetError unavailable", step);
            return;
        }
        EGLint code = get_error();
        emitf(emit, ctx, "egl: %s - eglGetError=%s 0x%04x", step,
              egl_error_name(code), (unsigned)code);
    };

    if (!get_display) {
        dlclose(handle);
        return GL_PROBE_EXIT_UNUSABLE;
    }

    EGLDisplay display = get_display(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY)
        emitf(emit, ctx, "egl: eglGetDisplay(EGL_DEFAULT_DISPLAY) -> EGL_NO_DISPLAY");
    else
        emitf(emit, ctx, "egl: eglGetDisplay(EGL_DEFAULT_DISPLAY) -> %p", display);
    report_error("after eglGetDisplay");

    if (!initialize) {
        dlclose(handle);
        return GL_PROBE_EXIT_UNUSABLE;
    }

    /*
     * Called even when the display came back EGL_NO_DISPLAY. The spec's answer
     * to that is a defined EGL_BAD_DISPLAY rather than undefined behaviour, and
     * "the display handle was already bad" is exactly the distinction this
     * probe exists to draw.
     */
    EGLint major = -1, minor = -1;
    EGLBoolean ok = initialize(display, &major, &minor);
    emitf(emit, ctx, "egl: eglInitialize -> %s (EGL %d.%d)",
          ok ? "true" : "FALSE", major, minor);
    report_error("after eglInitialize");

    if (!ok) {
        dlclose(handle);
        return GL_PROBE_EXIT_UNUSABLE;
    }

    if (query_string) {
        const char *vendor  = query_string(display, EGL_VENDOR);
        const char *version = query_string(display, EGL_VERSION_STRING);
        const char *apis    = query_string(display, EGL_CLIENT_APIS);
        emitf(emit, ctx, "egl: EGL_VENDOR=%s", vendor ? vendor : "(null)");
        emitf(emit, ctx, "egl: EGL_VERSION=%s", version ? version : "(null)");
        emitf(emit, ctx, "egl: EGL_CLIENT_APIS=%s", apis ? apis : "(null)");
    }

    if (terminate)
        emitf(emit, ctx, "egl: eglTerminate -> %s",
              terminate(display) ? "true" : "FALSE");

    dlclose(handle);
    return 0;
}

/* -------------------------------------------- tier 3: full dependency audit */

/*
 * dlerror() names the first unresolved dependency and stops. On a firmware
 * missing a whole set of 32-bit libraries that turns one run into a
 * conversation: install the library it named, run again, learn the next one.
 *
 * Reading DT_NEEDED out of the ELF gives the entire list at once, and trying
 * each name through dlopen() says which of them this system can actually
 * provide. That is the complete shopping list, from a single log.
 */
static int read_at(const unsigned char *base, size_t size, size_t offset,
                   void *out, size_t len)
{
    if (offset > size || size - offset < len)
        return 0;
    memcpy(out, base + offset, len);
    return 1;
}

/* A string out of the mapped file, refused unless it is NUL-terminated inside. */
static const char *string_at(const unsigned char *base, size_t size, size_t offset)
{
    if (offset >= size)
        return NULL;
    const void *end = memchr(base + offset, '\0', size - offset);
    return end ? (const char *)(base + offset) : NULL;
}

int gl_probe_deps(const char *path, gl_probe_report_fn emit, void *ctx)
{
    /*
     * The caller may hand over a soname rather than a path - SDL's default EGL
     * library is "libEGL.so.1" and nothing else. Reading the ELF needs a file,
     * so ask the runtime linker where that name landed. This only works when
     * the library loads; when it does not, the launcher's own audit runs on the
     * candidate's full path, which is the case that matters.
     */
    char resolved[PATH_MAX];
    if (!strchr(path, '/')) {
        void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            struct link_map *map_info = NULL;
            if (dlinfo(handle, RTLD_DI_LINKMAP, &map_info) == 0 && map_info &&
                map_info->l_name && *map_info->l_name) {
                snprintf(resolved, sizeof(resolved), "%s", map_info->l_name);
                emitf(emit, ctx, "deps: %s resolves to %s", path, resolved);
                path = resolved;
            }
            dlclose(handle);
        } else {
            emitf(emit, ctx, "deps: %s is a soname this system cannot load, so "
                             "there is no file to read its dependencies from",
                  path);
            return GL_PROBE_EXIT_UNUSABLE;
        }
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        emitf(emit, ctx, "deps: cannot open %s - %s", path, strerror(errno));
        return GL_PROBE_EXIT_UNUSABLE;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        emitf(emit, ctx, "deps: cannot stat %s - %s", path, strerror(errno));
        close(fd);
        return GL_PROBE_EXIT_UNUSABLE;
    }

    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        emitf(emit, ctx, "deps: cannot map %s - %s", path, strerror(errno));
        return GL_PROBE_EXIT_UNUSABLE;
    }
    const unsigned char *base = (const unsigned char *)map;

    /* Every structure below is memcpy'd out rather than cast in place: the
     * file is mapped at whatever alignment it has, and these are not. */
    Elf32_Ehdr eh;
    if (!read_at(base, size, 0, &eh, sizeof(eh)) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
        emitf(emit, ctx, "deps: %s is not an ELF file", path);
        munmap(map, size);
        return GL_PROBE_EXIT_UNUSABLE;
    }

    /*
     * Worth printing even when it is the expected answer. A 64-bit blob sitting
     * in a 32-bit library directory explains every downstream symptom at once,
     * and it is invisible to ldd on a 64-bit rootfs.
     */
    emitf(emit, ctx, "deps: %s is ELF%s, machine 0x%02x%s", path,
          eh.e_ident[EI_CLASS] == ELFCLASS64 ? "64" : "32",
          (unsigned)eh.e_machine,
          eh.e_machine == EM_ARM ? " (ARM)" : "");

    if (eh.e_ident[EI_CLASS] != ELFCLASS32) {
        emitf(emit, ctx, "deps: not a 32-bit object - this process could never "
                         "load it, whatever its dependencies are");
        munmap(map, size);
        return GL_PROBE_EXIT_UNUSABLE;
    }

    /* PT_DYNAMIC holds the tags; PT_LOAD is what turns a vaddr into an offset. */
    size_t dynamic_off = 0, dynamic_size = 0;
    Elf32_Phdr loads[16];
    size_t load_count = 0;

    for (unsigned i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        size_t off = (size_t)eh.e_phoff + (size_t)i * eh.e_phentsize;
        if (!read_at(base, size, off, &ph, sizeof(ph)))
            break;
        if (ph.p_type == PT_DYNAMIC) {
            dynamic_off = ph.p_offset;
            dynamic_size = ph.p_filesz;
        } else if (ph.p_type == PT_LOAD && load_count < 16) {
            loads[load_count++] = ph;
        }
    }

    if (!dynamic_size) {
        emitf(emit, ctx, "deps: %s has no PT_DYNAMIC - it declares no "
                         "dependencies at all", path);
        munmap(map, size);
        return 0;
    }

    auto vaddr_to_offset = [&](Elf32_Addr vaddr, size_t *out) {
        for (size_t i = 0; i < load_count; i++) {
            if (vaddr >= loads[i].p_vaddr &&
                vaddr - loads[i].p_vaddr < loads[i].p_filesz) {
                *out = loads[i].p_offset + (vaddr - loads[i].p_vaddr);
                return true;
            }
        }
        return false;
    };

    /* Two passes: DT_STRTAB may appear after the entries that need it. */
    size_t strtab_off = 0;
    bool have_strtab = false;
    for (size_t off = 0; off + sizeof(Elf32_Dyn) <= dynamic_size;
         off += sizeof(Elf32_Dyn)) {
        Elf32_Dyn dyn;
        if (!read_at(base, size, dynamic_off + off, &dyn, sizeof(dyn)))
            break;
        if (dyn.d_tag == DT_NULL)
            break;
        if (dyn.d_tag == DT_STRTAB)
            have_strtab = vaddr_to_offset(dyn.d_un.d_ptr, &strtab_off);
    }

    if (!have_strtab) {
        emitf(emit, ctx, "deps: %s has no readable DT_STRTAB", path);
        munmap(map, size);
        return GL_PROBE_EXIT_UNUSABLE;
    }

    int needed = 0, missing = 0;
    for (size_t off = 0; off + sizeof(Elf32_Dyn) <= dynamic_size;
         off += sizeof(Elf32_Dyn)) {
        Elf32_Dyn dyn;
        if (!read_at(base, size, dynamic_off + off, &dyn, sizeof(dyn)))
            break;
        if (dyn.d_tag == DT_NULL)
            break;

        if (dyn.d_tag != DT_NEEDED && dyn.d_tag != DT_SONAME &&
            dyn.d_tag != DT_RPATH && dyn.d_tag != DT_RUNPATH)
            continue;

        const char *name = string_at(base, size, strtab_off + dyn.d_un.d_val);
        if (!name)
            continue;

        if (dyn.d_tag == DT_SONAME) {
            emitf(emit, ctx, "deps: soname %s", name);
            continue;
        }
        if (dyn.d_tag == DT_RPATH || dyn.d_tag == DT_RUNPATH) {
            /* A driver whose RUNPATH points at a directory the firmware does
             * not have fails in a way that reads as a missing library. */
            emitf(emit, ctx, "deps: %s %s",
                  dyn.d_tag == DT_RPATH ? "rpath" : "runpath", name);
            continue;
        }

        needed++;
        dlerror();
        void *handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            emitf(emit, ctx, "deps: %s -> found", name);
            dlclose(handle);
        } else {
            const char *reason = dlerror();
            emitf(emit, ctx, "deps: %s -> MISSING (%s)", name,
                  reason ? reason : "no reason given");
            missing++;
        }
    }

    emitf(emit, ctx, "deps: %d needed, %d missing", needed, missing);
    munmap(map, size);
    return missing ? GL_PROBE_EXIT_UNUSABLE : 0;
}

/* ------------------------------------------------------------ command line */

int gl_probe_main(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: deadspace --gl-probe <library> [symbol ...]\n");
        return 2;
    }

    char error[512];
    int status = gl_probe_load(argv[0], (const char *const *)argv + 1, argc - 1,
                               error, sizeof(error));
    if (status != 0)
        printf("%s\n", error);
    return status;
}
