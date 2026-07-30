/*
 * fstat, in bionic's layout.
 *
 * The fourth host-vs-bionic struct mismatch of this port, and the one
 * progress.md has been predicting since iteration 2. The generated bionic
 * table binds fstat straight through to the host's, but the two sides do not
 * agree on what a `struct stat` is:
 *
 *   - bionic/armeabi-v7a uses the kernel's stat64 layout, 104 bytes, with
 *     32-bit time fields;
 *   - Debian trixie armhf is a 64-bit-time_t target, so the host's struct is
 *     a different size *and* has its members at different offsets.
 *
 * The game declares one on its stack and reads st_size out of offset 48. With
 * the host writing its own layout, that offset holds something else entirely,
 * and whatever the engine then does with the "size" of an asset - allocate it,
 * read into it, index it - is operating on a number that was never a size.
 *
 * So the fields are copied one at a time. There is no memcpy anywhere here on
 * purpose: the whole failure mode being fixed is the assumption that these two
 * structures have anything in common.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <string.h>

#include "so_util.h"
#include "thunk_gen.h"

/*
 * bionic/libc/include/sys/stat.h, __arm__ branch. The padding is part of the
 * ABI - the kernel's stat64 has those holes and bionic reproduces them - so it
 * is spelled out rather than left to the compiler.
 */
struct bionic_stat {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime_;
    uint32_t st_atime_nsec;
    uint32_t st_mtime_;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime_;
    uint32_t st_ctime_nsec;
    uint64_t st_ino;
};

static_assert(sizeof(struct bionic_stat) == 104,
              "bionic's struct stat is 104 bytes on armeabi-v7a; "
              "the game writes into a frame that size");
static_assert(offsetof(struct bionic_stat, st_size) == 48,
              "st_size at 48 is what the game's generated code indexes");

extern "C" int bionic_fstat(int fd, struct bionic_stat *out)
{
    if (!out)
        return -1;

    /* stat64 explicitly: the host's plain struct stat is the time64 one, and
     * its off_t is only as wide as the build's _FILE_OFFSET_BITS. */
    struct stat64 host;
    int rc = fstat64(fd, &host);
    if (rc != 0)
        return rc;

    memset(out, 0, sizeof(*out));
    out->st_dev         = (uint64_t)host.st_dev;
    out->__st_ino       = (uint32_t)host.st_ino;
    out->st_mode        = (uint32_t)host.st_mode;
    out->st_nlink       = (uint32_t)host.st_nlink;
    out->st_uid         = (uint32_t)host.st_uid;
    out->st_gid         = (uint32_t)host.st_gid;
    out->st_rdev        = (uint64_t)host.st_rdev;
    out->st_size        = (int64_t)host.st_size;
    out->st_blksize     = (uint32_t)host.st_blksize;
    out->st_blocks      = (uint64_t)host.st_blocks;
    /* Narrowed to 32 bits, like every other time_t in this port: the game was
     * compiled against a 32-bit time_t and reads these as such. */
    out->st_atime_      = (uint32_t)host.st_atim.tv_sec;
    out->st_atime_nsec  = (uint32_t)host.st_atim.tv_nsec;
    out->st_mtime_      = (uint32_t)host.st_mtim.tv_sec;
    out->st_mtime_nsec  = (uint32_t)host.st_mtim.tv_nsec;
    out->st_ctime_      = (uint32_t)host.st_ctim.tv_sec;
    out->st_ctime_nsec  = (uint32_t)host.st_ctim.tv_nsec;
    out->st_ino         = (uint64_t)host.st_ino;

    return 0;
}

DynLibFunction symtable_stat[] = {
    NO_THUNK("fstat", (uintptr_t)&bionic_fstat),
    { NULL, 0 },
};
