/*
 * Path-translating wrappers around the libc entry points the game opens files
 * with.
 *
 * ---------------------------------------------------------------------------
 * What was observed
 *
 * With com/eamobile/Query registered (see jni/classes/eamobile_Query.cpp) the
 * engine finally gets past its content gate and starts loading. Under
 * `qemu-arm -strace` the very first thing it does is:
 *
 *     openat(AT_FDCWD,"appbundle:/EAMCore.ini",O_RDONLY) = -1 ENOENT
 *     openat(AT_FDCWD,"appbundle:/eamcore.ini",O_RDONLY) = -1 ENOENT
 *
 * That is a literal Android URI reaching the host kernel. Two things are worth
 * naming about it:
 *
 *   - it is proof the three binary patches in src/patch.cpp do what they claim.
 *     Before them this path went out through JNI to AssetManager.open(); the
 *     open() syscall above is the fd path they switch the engine onto, and it
 *     is the reason those patches were kept even though they moved no
 *     milestone at the time.
 *   - the engine does NOT strip the scheme itself. On Android it never had to:
 *     "appbundle:" was the flag that sent the name down the AssetManager path,
 *     and the AssetManager was handed the remainder. With that branch patched
 *     out, whoever answers open() has to do the stripping.
 *
 * ---------------------------------------------------------------------------
 * The rules
 *
 * Taken from fix_path() in vita-ref/loader/reimpl/io.c, which runs this exact
 * build, rather than invented here:
 *
 *   appbundle:/X                            -> <gamedir>/assets/X
 *   .../Android/data/com.ea.deadspace/files/ -> that prefix deleted
 *   deadspace/published                      -> deadspace/assets/published
 *
 * The second and third exist because the engine also builds absolute paths out
 * of the app data directory it thinks it has. They are applied in that order
 * and are not mutually exclusive: a path can need the prefix removed and then
 * the published/ rewrite.
 *
 * One deliberate divergence from the reference. After those rules a path can
 * still be rooted somewhere that does not exist here (the Vita port's data
 * directory is a constant; ours is argv[1] and only known at runtime), so a
 * path that still names "deadspace/assets/" anywhere is re-rooted onto the
 * game directory. Without that, any name the engine derives from
 * GetAppDataDirectory - a JNI call this port answers with NULL - would land
 * outside the mounted tree and fail the same silent ENOENT way the scheme did.
 *
 * ---------------------------------------------------------------------------
 * Why wrappers and not a chdir or a symlink farm
 *
 * "appbundle:" is not a path at all, so no amount of cwd or mount trickery
 * reaches it; it has to be rewritten by something. And the rewrite has to
 * happen on the libc boundary rather than by patching the engine's string
 * building, because the same names are built in several places (the ini
 * loader, the package reader, the save system) and only one of them was ever
 * disassembled.
 *
 * No allocation happens in here. The engine calls open() from inside its own
 * allocator's core-acquisition path, so a fix_path() that malloc'd - as the
 * reference one does - would re-enter an allocator that is mid-update.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <atomic>

#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"
#include "fix_path.h"

/* argv[1]. Static storage rather than a pointer into argv because the thunks
 * outlive nothing in particular but the value has to be readable from any
 * thread the engine starts. */
static char g_game_dir[PATH_MAX] = "/game";
static std::atomic<long> g_assets_opened(0);

void io_set_game_dir(const char *dir)
{
    if (dir && *dir)
        snprintf(g_game_dir, sizeof(g_game_dir), "%s", dir);
}

const char *io_game_dir(void) { return g_game_dir; }

static const char kScheme[]  = "appbundle:/";
static const char kAndroid[] = "Android/data/com.ea.deadspace/files/";

const char *fix_path(const char *orig, char *buf, size_t bufsz)
{
    if (!orig || !*orig)
        return orig;

    /* Rule 1: the scheme. Matched with strstr, not a prefix test, because the
     * engine sometimes concatenates it onto a directory it already built. */
    const char *scheme = strstr(orig, kScheme);
    if (scheme) {
        snprintf(buf, bufsz, "%s/assets/%s", g_game_dir,
                 scheme + sizeof(kScheme) - 1);
        return buf;
    }

    /* Rules 2 and 3 both operate on the same working copy. */
    char work[PATH_MAX];
    snprintf(work, sizeof(work), "%s", orig);
    bool changed = false;

    char *android = strstr(work, kAndroid);
    if (android) {
        memmove(android, android + sizeof(kAndroid) - 1,
                strlen(android + sizeof(kAndroid) - 1) + 1);
        changed = true;
    }

    /*
     * Rule 3, expressed against the game directory rather than as a literal.
     *
     * The reference spells it "deadspace/published" -> "deadspace/assets/
     * published" because its data root is a compile-time constant ending in
     * "deadspace". Ours is argv[1], handed to the engine by
     * com/ea/blast/GetAppDataDirectoryDelegate, so the same rewrite has to key
     * off that: the engine believes published/ hangs off the root, and in the
     * extracted tree it hangs off assets/.
     */
    size_t root_len = strlen(g_game_dir);
    if (strncmp(work, g_game_dir, root_len) == 0 &&
        strncmp(work + root_len, "/published", 10) == 0) {
        char tail[PATH_MAX];
        snprintf(tail, sizeof(tail), "%s", work + root_len);
        snprintf(work, sizeof(work), "%s/assets%s", g_game_dir, tail);
        changed = true;
    }

    /* The literal form as well, for any path the engine built before it had a
     * root from us - it still names the vendor directory. */
    char *published = strstr(work, "deadspace/published");
    if (published) {
        char tail[PATH_MAX];
        snprintf(tail, sizeof(tail), "%s", published + strlen("deadspace/"));
        snprintf(published, sizeof(work) - (size_t)(published - work),
                 "deadspace/assets/%s", tail);
        changed = true;
    }

    /* The divergence described above: re-root anything still pointing at an
     * asset tree we are not mounted at. */
    char *assets = strstr(work, "deadspace/assets/");
    if (assets) {
        snprintf(buf, bufsz, "%s/%s", g_game_dir,
                 assets + strlen("deadspace/"));
        return buf;
    }

    if (!changed)
        return orig;

    snprintf(buf, bufsz, "%s", work);
    return buf;
}

/*
 * Rate-limited so a game that probes for a hundred optional files does not
 * bury the rest of the log, but not silent: the first translations are how the
 * next reader learns which names the engine actually asks for. Everything past
 * the cap is still counted in the summary line.
 */
static void trace_path(const char *what, const char *orig, const char *fixed, int rc)
{
    static int shown = 0;
    if (fixed == orig || shown >= 24)
        return;
    shown++;
    trace("io: %s '%s' -> '%s' (%s)", what, orig, fixed,
          rc >= 0 ? "ok" : "ENOENT");
}

/*
 * Count successful content-file opens, not probes, directories, saves or
 * configuration. The path has already passed through fix_path(), so the
 * extracted asset tree is the stable boundary regardless of which spelling
 * the engine used (appbundle:, Android/data/... or its VFS mount).
 */
static void count_asset_open(const char *fixed, bool succeeded)
{
    if (succeeded && fixed && strstr(fixed, "/assets/published/"))
        g_assets_opened.fetch_add(1, std::memory_order_relaxed);
}

/* Declared in thunks/libc/generated/impl_header.h, which pulls in the whole
 * generated prototype set; only this one is needed, so it is re-declared with
 * matching C++ linkage instead. */
extern ABI_ATTR void *fopen_impl(const char *path, const char *mode);

extern "C" {

/*
 * open() is variadic in the host header but not in what the game emits: it
 * passes two registers for a read and three for a create. Declaring the third
 * unconditionally is safe on AAPCS - r2 is caller-saved scratch either way -
 * and avoids a va_list in a function the allocator can reach.
 */
int bionic_open(const char *path, int flags, mode_t mode)
{
    char buf[PATH_MAX];
    const char *fixed = fix_path(path, buf, sizeof(buf));
    int fd = open(fixed, flags, mode);
    count_asset_open(fixed, fd >= 0);
    trace_path("open", path, fixed, fd);
    return fd;
}

/*
 * Forwarded to the existing thunk rather than calling the host's fopen.
 *
 * fopen_impl() does not return a host FILE*: it wraps one in a BIONIC_FILE, a
 * structure with bionic's field offsets, because the game's inlined getc/putc
 * read _p and _r out of it directly. Re-implementing the open here would hand
 * back a host FILE* that looks fine until the first inlined character read.
 * Only the path is ours to change.
 */
void *bionic_fopen(const char *path, const char *mode)
{
    char buf[PATH_MAX];
    const char *fixed = fix_path(path, buf, sizeof(buf));
    void *f = fopen_impl(fixed, mode);
    count_asset_open(fixed, f != NULL);
    trace_path("fopen", path, fixed, f ? 0 : -1);
    return f;
}

long android_io_assets_opened(void)
{
    return g_assets_opened.load(std::memory_order_relaxed);
}

void *bionic_opendir(const char *path)
{
    char buf[PATH_MAX];
    const char *fixed = fix_path(path, buf, sizeof(buf));
    DIR *d = opendir(fixed);
    trace_path("opendir", path, fixed, d ? 0 : -1);
    return d;
}

int bionic_mkdir(const char *path, mode_t mode)
{
    char buf[PATH_MAX];
    return mkdir(fix_path(path, buf, sizeof(buf)), mode);
}

int bionic_remove(const char *path)
{
    char buf[PATH_MAX];
    return remove(fix_path(path, buf, sizeof(buf)));
}

int bionic_unlink(const char *path)
{
    char buf[PATH_MAX];
    return unlink(fix_path(path, buf, sizeof(buf)));
}

int bionic_rename(const char *from, const char *to)
{
    char a[PATH_MAX], b[PATH_MAX];
    /* Two buffers: fix_path may return either of them or the original, so they
     * cannot be shared between the two arguments. */
    const char *fa = fix_path(from, a, sizeof(a));
    const char *fb = fix_path(to,   b, sizeof(b));
    return rename(fa, fb);
}

} /* extern "C" */

DynLibFunction symtable_io[] = {
    /* fopen returns a FILE* the game only ever hands back to us, so the
     * bionic/host FILE layout difference does not matter here - unlike the
     * struct stat in symtab_stat.cpp, which the game reads fields out of. */
    THUNK_SPECIFIC("open",    bionic_open),
    THUNK_SPECIFIC("fopen",   bionic_fopen),
    THUNK_SPECIFIC("opendir", bionic_opendir),
    THUNK_SPECIFIC("mkdir",   bionic_mkdir),
    THUNK_SPECIFIC("remove",  bionic_remove),
    THUNK_SPECIFIC("unlink",  bionic_unlink),
    THUNK_SPECIFIC("rename",  bionic_rename),
    { NULL, 0 },
};
