/*
 * GL provider loadability preflight.
 *
 * The launcher picks the device's 32-bit GL stack by looking for files. A file
 * that exists is not a driver that works: a muOS device with a 64-bit userland
 * shipped /usr/lib32/libmali.so.0 whose 32-bit dependencies were never
 * installed, so SDL reported "Can't load EGL/GL library on window creation",
 * every GLES1 import resolved to nil and the loader died with 182 missing
 * implementations.
 *
 * Only a real dlopen from a 32-bit process answers "can this be loaded?", and
 * this binary is the one 32-bit process the port is guaranteed to have. Hence
 * the subcommand: the launcher runs `deadspace --gl-probe <library> [symbol
 * ...]` before committing to a candidate, and falls through to the next tier
 * when it fails.
 */
#ifndef DEADSPACE_GL_PROBE_H
#define DEADSPACE_GL_PROBE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Exit status meaning "the probe ran and the library is not usable". Distinct
 * from 0 (usable) and from every status the shell produces when the probe
 * itself could not run (126/127) or was misused (2), so the launcher can tell
 * a verdict apart from a malfunction and only ever reject on a verdict.
 */
#define GL_PROBE_EXIT_UNUSABLE 3

/*
 * The probes report line by line through this, so the same code serves the
 * subcommands (printing to stdout, for the launcher to capture into log.txt)
 * and the loader's own failure path (routing them through trace()).
 */
typedef void (*gl_probe_report_fn)(void *ctx, const char *line);

/* The sink the subcommands use. */
void gl_probe_report_stdout(void *ctx, const char *line);

/*
 * Tier 1 - can it be loaded at all. Returns 0 or GL_PROBE_EXIT_UNUSABLE and
 * writes the dlerror() text into `error`.
 */
int gl_probe_load(const char *path, const char *const *symbols, int symbol_count,
                  char *error, size_t error_size);

/*
 * Tier 2 - does EGL come up. dlopen is not where the muOS device dies: the
 * blob loads and SDL still answers "Can't load EGL/GL library on window
 * creation", so the failure is inside bring-up. This walks the same path SDL
 * does - eglGetDisplay, eglInitialize, the vendor strings - and reports each
 * step's outcome and eglGetError() by name, so the log says which step failed
 * rather than that something did.
 */
int gl_probe_init(const char *path, gl_probe_report_fn emit, void *ctx);

/*
 * Tier 3 - what does it need that this system does not have. dlerror() names
 * only the first missing dependency; this reads DT_NEEDED out of the ELF and
 * dlopens every entry, so one run yields the complete list.
 */
int gl_probe_deps(const char *path, gl_probe_report_fn emit, void *ctx);

/*
 * argv[0] is the library path, argv[1..] are symbols that must resolve.
 * Prints the dlerror() text on failure. Returns a process exit status.
 */
int gl_probe_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPACE_GL_PROBE_H */
