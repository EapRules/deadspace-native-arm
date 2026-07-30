#include <stdio.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "fix_path.h"
#include "ea_GetAppDataDirectoryDelegate.h"

/*
 * The engine's data root.
 *
 * ---------------------------------------------------------------------------
 * How this one announced itself
 *
 * After the semaphore fix the run got as far as reading EAMCore.ini and then
 * died at text+0x31e5a8 with a null dereference. `qemu-arm -strace` showed the
 * two syscalls immediately before it:
 *
 *     openat("/game/assets/EAMCore.ini", O_RDONLY) = 5
 *     read(5, ..., 10000) = 3119
 *     close(5)
 *     statx(AT_FDCWD, "(nil)/published/var", ...) = -1 ENOENT
 *     statx(AT_FDCWD, "(nil)/published/var", ...) = -1 ENOENT
 *     --- SIGSEGV
 *
 * "(nil)" is bionic's printf rendering a NULL %s. The engine had built its
 * data root by formatting the string these methods return, got NULL, and
 * carried the literal four characters into every path after it. The crash two
 * instructions later is the filesystem module's lazy-create returning NULL and
 * being dereferenced at once (+0x31e5a4: mov r0,r7 / ldr r3,[r7]) - a
 * consequence, not the defect.
 *
 * The class was not in the "no fake class registered" list in the log, which
 * is worth saying because it is the reason this took a syscall trace rather
 * than a grep: FindClass is only reached the first time a lookup happens, and
 * the engine resolves this one lazily, after the point where the previous
 * iteration's run had already stopped.
 *
 * The names are read out of the binary, at .rodata 0x3dacd8..0x3dad20:
 * "com/ea/blast/GetAppDataDirectoryDelegate", "GetAppDataDirectory",
 * "GetExternalStorageDirectory". The vita loader lists the same two methods as
 * ids 107 and 108 in jni_specific.h.
 *
 * ---------------------------------------------------------------------------
 * Why the game directory, and not a writable directory
 *
 * The obvious answer is "somewhere we can write", since the first thing the
 * engine does with the root is look for published/var, which is save data.
 * That is wrong here: the same root is what every asset path is built from,
 * and the assets are the 300 MB tree the player supplies. Pointing the root at
 * scratch space would fix the save probe and break everything else.
 *
 * So the root is the game directory, exactly as the Vita port does it - its
 * fix_path() rewrites "deadspace/published" into "deadspace/assets/published"
 * precisely because its root is the game directory too and the published tree
 * sits one level deeper than the engine believes. src/symtab_io.cpp does the
 * same rewrite, generalised to whatever argv[1] is.
 *
 * Writes under a read-only game directory will fail, and that is a separate
 * problem with a separate fix (a save redirect); it is deliberately not
 * conflated with this one. What matters for now is that the root is a real
 * path instead of four characters spelling "(nil)".
 */
/*
 * The constructor. There is nothing to construct - the delegate carries no
 * state - but it has to exist, because iface_NewObjectV refuses a class with
 * instance_size 0 and the engine calls NewObject before it calls anything
 * else.
 */
void GetAppDataDirectoryDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env;
    (void)obj;
    (void)clazz;
}

jstring GetAppDataDirectoryDelegate::GetAppDataDirectory(JNIEnv *env, jobject obj)
{
    (void)env;
    (void)obj;

    /* Cached: DeleteLocalRef is a no-op in this port, so a fresh String per
     * call would leak one per path the engine builds. Same reasoning as
     * xt_System.cpp's getDeviceInfo. */
    static String *cached = NULL;
    if (!cached) {
        cached = new String(io_game_dir());
        trace("GetAppDataDirectory -> '%s'", io_game_dir());
    }
    return (jstring)cached;
}

/*
 * The external storage directory is the same place.
 *
 * On a phone these differ - one is /data/data/<pkg>, the other /sdcard - and
 * the engine uses the second for anything large. Here there is one tree and
 * both names have to reach it, or half the asset paths would be built from a
 * root that has no assets under it.
 */
jstring GetAppDataDirectoryDelegate::GetExternalStorageDirectory(JNIEnv *env, jobject obj)
{
    return GetAppDataDirectory(env, obj);
}

/*
 * Registered through ManagedMethod::Register directly rather than through the
 * REGISTER_METHOD macro. The macro routes via DEDUCE_METHOD_HELPER, whose only
 * two overloads take (JNIEnv*, jobject, jclass, ...) and (JNIEnv*, jclass,
 * ...) - it cannot express a plain instance method, which is the
 * (JNIEnv*, jobject, ...) shape the engine calls these two with.
 */
const ManagedMethod GetAppDataDirectoryDelegateMethods[] = {
    ManagedMethod::RegisterNonVirtual<&GetAppDataDirectoryDelegate::ctor>(
        GetAppDataDirectoryDelegate::clazz, "<init>", "()V"),
    ManagedMethod::Register<&GetAppDataDirectoryDelegate::GetAppDataDirectory>(
        GetAppDataDirectoryDelegate::clazz,
        "GetAppDataDirectory", "()Ljava/lang/String;"),
    ManagedMethod::Register<&GetAppDataDirectoryDelegate::GetExternalStorageDirectory>(
        GetAppDataDirectoryDelegate::clazz,
        "GetExternalStorageDirectory", "()Ljava/lang/String;"),
    {NULL},
};

Class GetAppDataDirectoryDelegate::clazz = {
    .classpath        = "com/ea/blast/GetAppDataDirectoryDelegate",
    .classname        = "GetAppDataDirectoryDelegate",
    .managed_methods  = GetAppDataDirectoryDelegateMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(GetAppDataDirectoryDelegate),
};

static const int registered =
    ClassRegistry::register_class(GetAppDataDirectoryDelegate::clazz);
