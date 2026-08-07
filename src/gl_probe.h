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
 * argv[0] is the library path, argv[1..] are symbols that must resolve.
 * Prints the dlerror() text on failure. Returns a process exit status.
 */
int gl_probe_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPACE_GL_PROBE_H */
