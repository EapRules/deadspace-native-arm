#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "fix_path.h"
#include "android_assets.h"

/*
 * Why these classes exist at all, given src/patch.cpp already switched the
 * engine off the JNI I/O path.
 *
 * Those three patches make the "appbundle:" prefix test fail, which sends the
 * *content* reads down open()/fopen() - and that works today, the log shows
 * '/game/assets/EAMCore.ini' being opened through the libc wrappers. What they
 * do not touch is EAIO's bring-up, which happens before any of that: EAIO
 * fetches the activity, asks it for an AssetManager, hands that to its native
 * Startup, and then caches jmethodIDs for the six stream calls. With the
 * classes missing, every one of those came back NULL, EAIO started up around a
 * NULL asset manager, and the engine carried on with an I/O backend that is
 * half-initialised.
 *
 * The Vita port registers exactly these methods (loader/jni_specific.h ids
 * 219-221 and 600-606, loader/android/java.io.InputStream.c) even though it
 * applies the same three patches, which is the strongest evidence available
 * that the JNI side is still load-bearing after them.
 */

/*
 * One open asset at a time, in a file-scope descriptor.
 *
 * This is not a simplification, it is what the reference does and what the
 * engine's usage allows: openFd() is followed immediately by getLength() and a
 * read/close pair, on a single thread, before the next open. Handing the fd
 * back inside the returned object instead would be tidier, but read() and
 * skip() are looked up on the InputStream class and invoked with whichever
 * object the engine happens to hold - the stream from open(), or the
 * descriptor from openFd() - and there is no way to tell those apart from
 * inside the call. Keying off the object would therefore be a guess; keying
 * off "the asset that is currently open" is not.
 */
static int g_fd = -1;

/*
 * Asset name -> host path.
 *
 * Names arriving here are relative to the assets root ("published/...") but
 * the engine also passes fully-built names that still carry the appbundle
 * scheme, so everything goes through fix_path() first and only what comes out
 * still relative gets the assets root prepended.
 */
static const char *resolve_asset(const char *name, char *buf, size_t bufsz)
{
    if (!name || !*name)
        return NULL;

    char tmp[PATH_MAX];
    const char *fixed = fix_path(name, tmp, sizeof(tmp));

    if (fixed[0] == '/') {
        snprintf(buf, bufsz, "%s", fixed);
    } else {
        const char *rel = fixed;
        while (*rel == '/')
            rel++;
        snprintf(buf, bufsz, "%s/assets/%s", io_game_dir(), rel);
    }

    return buf;
}

/* ------------------------------------------------------------------ *
 * com/ea/blast/MainActivity
 * ------------------------------------------------------------------ */

/*
 * Both of these are singletons rather than fresh allocations per call.
 *
 * DeleteLocalRef is a no-op in this JNI, so anything handed out here is never
 * reclaimed; the engine re-fetches the activity whenever it needs the asset
 * manager, so a fresh object per call would leak one per lookup. The reference
 * returns a strdup'd string here for the same "must be a real, distinguishable
 * pointer" reason - a typed object costs the same and survives a GetObjectClass.
 */
jobject MainActivity::GetInstance(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;

    static MainActivity *instance = NULL;
    if (!instance) {
        instance = new MainActivity();
        trace("MainActivity.GetInstance -> %p (the fake activity)", (void *)instance);
    }
    return (jobject)instance;
}

jobject MainActivity::getAssets(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;

    static AssetManager *assets = NULL;
    if (!assets) {
        assets = new AssetManager();
        trace("MainActivity.getAssets -> %p (assets served from %s/assets)",
              (void *)assets, io_game_dir());
    }
    return (jobject)assets;
}

const ManagedMethod MainActivityMethods[] = {
    ManagedMethod::RegisterStatic<&MainActivity::GetInstance>(
        MainActivity::clazz, "GetInstance", "()Lcom/ea/blast/MainActivity;"),
    ManagedMethod::Register<&MainActivity::getAssets>(
        MainActivity::clazz, "getAssets", "()Landroid/content/res/AssetManager;"),
    {NULL},
};

Class MainActivity::clazz = {
    .classpath        = "com/ea/blast/MainActivity",
    .classname        = "MainActivity",
    .managed_methods  = MainActivityMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(MainActivity),
};

/* ------------------------------------------------------------------ *
 * java/io/InputStream (and the three AssetManager calls, see the header)
 * ------------------------------------------------------------------ */

jobject InputStream::openStream(JNIEnv *env, jobject obj, jstring name)
{
    (void)env; (void)obj;

    const char *asset = name ? ((String *)name)->str : NULL;
    char path[PATH_MAX];
    const char *resolved = resolve_asset(asset, path, sizeof(path));

    if (g_fd >= 0) {
        ::close(g_fd);
        g_fd = -1;
    }

    if (resolved)
        g_fd = ::open(resolved, O_RDONLY);

    trace("assets: open '%s' -> '%s' (%s)", asset ? asset : "(null)",
          resolved ? resolved : "(null)", g_fd >= 0 ? "ok" : "failed");

    /*
     * A failed open still has to answer with a non-NULL stream. On Android the
     * failure is an IOException, which this JNI cannot raise; returning NULL
     * instead puts the engine on a path where it calls read() on nothing.
     * Answering with the stream object and letting read() report -1 (end of
     * stream) is the closest reachable behaviour, and it is what the reference
     * does - it returns its dummy unconditionally.
     */
    static InputStream *stream = NULL;
    if (!stream)
        stream = new InputStream();
    return (jobject)stream;
}

jobject InputStream::openFd(JNIEnv *env, jobject obj, jstring name)
{
    (void)env; (void)obj;

    const char *asset = name ? ((String *)name)->str : NULL;
    char path[PATH_MAX];
    const char *resolved = resolve_asset(asset, path, sizeof(path));

    if (g_fd >= 0) {
        ::close(g_fd);
        g_fd = -1;
    }

    if (resolved)
        g_fd = ::open(resolved, O_RDONLY);

    trace("assets: openFd '%s' -> '%s' (fd=%d)", asset ? asset : "(null)",
          resolved ? resolved : "(null)", g_fd);

    /*
     * The reference hands the raw fd back here, cast to a jobject. That works
     * because the engine only ever passes it straight back in as `this` for
     * getLength() - but a small integer travelling as an object is one
     * mis-step away from being dereferenced or freed, and this port already
     * spent a run on a SIGSEGV that was exactly "free() of a pointer whose
     * value is 1". A real object costs nothing and cannot become that.
     */
    static AssetFileDescriptor *fd_obj = NULL;
    if (!fd_obj)
        fd_obj = new AssetFileDescriptor();
    return (jobject)fd_obj;
}

jint InputStream::read(JNIEnv *env, jobject obj, jbyteArray b, jint off, jint len)
{
    (void)env; (void)obj;

    if (g_fd < 0)
        return -1;
    if (len <= 0)
        return 0;

    ArrayObject *array = (ArrayObject *)b;
    if (!array || !array->elements)
        return -1;

    /* The engine sizes the array itself, but it is the only bound available
     * here and a short one would be a silent heap overwrite. */
    jsize capacity = array->count * array->element_size;
    if (off < 0 || off > capacity)
        return -1;
    if (len > capacity - off)
        len = capacity - off;

    ssize_t got = ::read(g_fd, (char *)array->elements + off, (size_t)len);
    if (got <= 0)
        return -1;   /* Java signals end-of-stream with -1, not 0. */

    return (jint)got;
}

jlong InputStream::skip(JNIEnv *env, jobject obj, jlong n)
{
    (void)env; (void)obj;

    if (g_fd < 0)
        return -1;

    off_t before = lseek(g_fd, 0, SEEK_CUR);
    off_t after  = lseek(g_fd, (off_t)n, SEEK_CUR);
    if (before < 0 || after < 0)
        return -1;

    /* The reference returns a flat 1 here and notes it is skipping the count
     * "for efficiency". The real number is two lseeks away and is what the
     * Java contract promises, so a caller that loops until it has skipped n
     * terminates instead of spinning. */
    return (jlong)(after - before);
}

void InputStream::closeStream(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;

    if (g_fd >= 0) {
        ::close(g_fd);
        g_fd = -1;
    }
}

/*
 * AssetManager.list().
 *
 * Android returns the bare names directly under `path`, including directory
 * names without a trailing slash. EAIO calls list("") and appends a slash to
 * extensionless entries before looking for "published/". Returning absolute
 * or recursive names prevents that exact match and leaves the asset backend
 * without its content root.
 */
static bool append_asset_name(const char *name, char ***out,
                              size_t *count, size_t *cap)
{
    if (*count == *cap) {
        size_t next = *cap ? *cap * 2 : 256;
        char **grown = (char **)realloc(*out, next * sizeof(char *));
        if (!grown)
            return false;
        *out = grown;
        *cap = next;
    }

    (*out)[*count] = strdup(name);
    if (!(*out)[*count])
        return false;
    (*count)++;
    return true;
}

static void list_level(const char *dir, char ***out,
                       size_t *count, size_t *cap)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        if (!append_asset_name(entry->d_name, out, count, cap))
            break;
    }

    closedir(d);
}

jobjectArray InputStream::listAssets(JNIEnv *env, jobject obj, jstring path)
{
    (void)env; (void)obj;

    const char *asset = path ? ((String *)path)->str : "";
    char dir[PATH_MAX];
    const char *resolved;
    if (*asset) {
        resolved = resolve_asset(asset, dir, sizeof(dir));
    } else {
        /*
         * Do not spell the asset root as "<root>/assets/.".
         *
         * The working Vita implementation concatenates its assets prefix with
         * the empty argument and therefore enumerates the root without this
         * extra component. Match that exact host path shape while enumerating
         * the first level.
         */
        snprintf(dir, sizeof(dir), "%s/assets", io_game_dir());
        resolved = dir;
    }

    char **names = NULL;
    size_t count = 0, cap = 0;
    if (resolved)
        list_level(resolved, &names, &count, &cap);

    trace("assets: list '%s' -> '%s' (%zu entries)", asset,
          resolved ? resolved : "(null)", count);

    /* Built by hand rather than through NewObjectArray + SetObjectArrayElement:
     * that pair stores elements *by value* (element_size = sizeof(String)), so
     * the array has to own the String objects, not pointers to them. */
    ArrayObject *array = (ArrayObject *)calloc(1, sizeof(ArrayObject));
    array->instance_clazz = &String::clazz;
    array->count          = (jsize)count;
    array->element_size   = sizeof(String);
    array->elements       = calloc(count ? count : 1, sizeof(String));

    for (size_t i = 0; i < count; i++) {
        /*
         * Select the copying constructor deliberately. names[i] is `char *`,
         * which otherwise selects String(char *) - the ownership-taking
         * overload - and the free directly below leaves every array element
         * pointing at released storage.
         */
        new ((char *)array->elements + i * sizeof(String))
            String((const char *)names[i]);
        free(names[i]);
    }
    free(names);

    return (jobjectArray)array;
}

const ManagedMethod InputStreamMethods[] = {
    ManagedMethod::Register<&InputStream::read>(
        InputStream::clazz, "read", "([BII)I"),
    ManagedMethod::Register<&InputStream::closeStream>(
        InputStream::clazz, "close", "()V"),
    ManagedMethod::Register<&InputStream::skip>(
        InputStream::clazz, "skip", "(J)J"),
    ManagedMethod::Register<&InputStream::openStream>(
        InputStream::clazz, "open", "(Ljava/lang/String;)Ljava/io/InputStream;"),
    ManagedMethod::Register<&InputStream::openFd>(
        InputStream::clazz, "openFd",
        "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;"),
    ManagedMethod::Register<&InputStream::listAssets>(
        InputStream::clazz, "list", "(Ljava/lang/String;)[Ljava/lang/String;"),
    {NULL},
};

Class InputStream::clazz = {
    .classpath        = "java/io/InputStream",
    .classname        = "InputStream",
    .managed_methods  = InputStreamMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(InputStream),
};

/* Same method table: the engine never looks a method up on this class - it
 * uses the InputStream jclass for all six - but the object getAssets() returns
 * has to answer GetObjectClass with something, and something that can dispatch
 * open/openFd/list is strictly safer than something that cannot. */
Class AssetManager::clazz = {
    .classpath        = "android/content/res/AssetManager",
    .classname        = "AssetManager",
    .managed_methods  = InputStreamMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(AssetManager),
};

/* ------------------------------------------------------------------ *
 * android/content/res/AssetFileDescriptor
 * ------------------------------------------------------------------ */

jlong AssetFileDescriptor::getLength(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;

    if (g_fd < 0)
        return -1;

    off_t here = lseek(g_fd, 0, SEEK_CUR);
    off_t end  = lseek(g_fd, 0, SEEK_END);
    lseek(g_fd, here, SEEK_SET);

    if (end < 0)
        return -1;

    /* Android rewinds nothing; the reference seeks back to 0 instead of to
     * where the engine was, which loses the position when the engine measures
     * a stream it has already started reading. Restoring `here` is the same
     * answer without that side effect. */
    return (jlong)end;
}

const ManagedMethod AssetFileDescriptorMethods[] = {
    ManagedMethod::Register<&AssetFileDescriptor::getLength>(
        AssetFileDescriptor::clazz, "getLength", "()J"),
    {NULL},
};

Class AssetFileDescriptor::clazz = {
    .classpath        = "android/content/res/AssetFileDescriptor",
    .classname        = "AssetFileDescriptor",
    .managed_methods  = AssetFileDescriptorMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(AssetFileDescriptor),
};

static const int registered_activity = ClassRegistry::register_class(MainActivity::clazz);
static const int registered_assets   = ClassRegistry::register_class(AssetManager::clazz);
static const int registered_stream   = ClassRegistry::register_class(InputStream::clazz);
static const int registered_afd      = ClassRegistry::register_class(AssetFileDescriptor::clazz);
