/*
 * AAssetManager implemented straight on top of the APK zip.
 *
 * The game opens ~1180 files through this API (287 .pod models, 778 .wav,
 * shaders, atlases). Everything under assets/ in the APK is served from here,
 * so no extraction step is needed and the user keeps a single .apk file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <atomic>
#include <vector>
#include <string>

#include <zip.h>

#include "android_api.h"
#include "trace.h"

#define ASSET_PREFIX "assets/"

/*
 * How many assets the engine actually pulled out of the APK.
 *
 * This is the only honest place to count them: it is incremented once per
 * AAssetManager_open that really found the entry and really read its bytes,
 * so a lookup that missed, or a path the game asks for and does not get, never
 * shows up here. Written by the game's thread, read by the loader's when the
 * run ends - hence the atomic.
 */
static std::atomic<long> g_assets_opened(0);

extern "C" long android_assets_opened(void) { return g_assets_opened.load(); }

/* The zip handle is owned by main.cpp; assets borrow it. */
static zip_t *g_apk = NULL;

extern "C" void android_assets_init(zip_t *apk)
{
    g_apk = apk;
}

/*
 * There is exactly one asset source (the APK), so every AAssetManager* the
 * game receives is the same opaque token and none of the AAssetManager_*
 * entry points read through it. It still has to be non-null: the game checks
 * ANativeActivity::assetManager before using it, and treats null as "no
 * assets" rather than crashing, which would look like a corrupt APK.
 */
static struct AAssetManager *const g_asset_manager =
    (struct AAssetManager *)(void *)&g_apk;

extern "C" AAssetManager *android_assets_manager(void)
{
    return g_asset_manager;
}

/* ------------------------------------------------------------------ *
 * AAsset — always fully buffered.
 *
 * AAsset_getBuffer() must hand back a stable pointer to the whole file, and
 * zip entries are deflated, so there is no way to mmap them. Reading the entry
 * up front is the only correct option. The biggest asset here is a 5 MB ogg,
 * which is fine on a 1 GB device.
 * ------------------------------------------------------------------ */
struct AAsset {
    unsigned char *data;
    size_t         size;
    size_t         pos;
    int            tmp_fd;    /* only created if openFileDescriptor is called */
    char          *tmp_path;
};

struct AAssetDir {
    std::vector<std::string> entries;
    size_t                   next;
};

static bool zip_slurp(const char *path, unsigned char **out, size_t *out_size)
{
    zip_stat_t st;
    if (zip_stat(g_apk, path, 0, &st) != 0)
        return false;
    if (!(st.valid & ZIP_STAT_SIZE))
        return false;

    zip_file_t *zf = zip_fopen(g_apk, path, 0);
    if (!zf)
        return false;

    unsigned char *buf = (unsigned char *)malloc(st.size + 1);
    if (!buf) {
        zip_fclose(zf);
        return false;
    }

    zip_int64_t got = zip_fread(zf, buf, st.size);
    zip_fclose(zf);

    if (got < 0 || (zip_uint64_t)got != st.size) {
        free(buf);
        return false;
    }

    buf[st.size] = 0;   /* text assets (json, shaders) are read as strings */
    *out = buf;
    *out_size = st.size;
    return true;
}

/*
 * Asset override.
 *
 * The game's splash is 1280x720 and the panel is 640x480, so it sits letter-
 * boxed. Rather than ask anyone to repack their own APK to change one image,
 * a file dropped in the override directory wins over the APK entry of the same
 * name. DEADSPACE_ASSET_OVERRIDE points at it; the launcher sets it to
 * <port>/assets. Absent directory, absent file: the APK answers as before.
 */
static bool override_slurp(const char *filename, unsigned char **out, size_t *out_size)
{
    const char *dir = getenv("DEADSPACE_ASSET_OVERRIDE");
    if (!dir || !*dir)
        return false;

    std::string path = std::string(dir) + "/" + filename;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return false; }
    rewind(f);

    unsigned char *buf = (unsigned char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return false; }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return false; }

    buf[size] = 0;
    *out = buf;
    *out_size = (size_t)size;
    trace("asset override: %s (%ld bytes)", filename, size);
    return true;
}

extern "C" AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode)
{
    (void)mgr; (void)mode;
    if (!g_apk || !filename)
        return NULL;

    {
        unsigned char *odata = NULL;
        size_t osize = 0;
        if (override_slurp(filename, &odata, &osize)) {
            AAsset *a = (AAsset *)calloc(1, sizeof(AAsset));
            if (!a) { free(odata); return NULL; }
            a->data = odata; a->size = osize; a->tmp_fd = -1;
            g_assets_opened++;
            return a;
        }
    }

    std::string path = std::string(ASSET_PREFIX) + filename;

    unsigned char *data = NULL;
    size_t size = 0;
    if (!zip_slurp(path.c_str(), &data, &size))
        return NULL;

    AAsset *asset = (AAsset *)calloc(1, sizeof(AAsset));
    if (!asset) {
        free(data);
        return NULL;
    }
    asset->data   = data;
    asset->size   = size;
    asset->tmp_fd = -1;

    g_assets_opened++;
    return asset;
}

extern "C" const void *AAsset_getBuffer(AAsset *asset)
{
    return asset ? asset->data : NULL;
}

extern "C" off_t AAsset_getLength(AAsset *asset)
{
    return asset ? (off_t)asset->size : 0;
}

/*
 * Some audio paths want a real file descriptor. A zip entry has none, so spill
 * the buffer to a temp file once and hand back that fd. outStart is always 0
 * because the temp file holds this asset alone.
 */
extern "C" int AAsset_openFileDescriptor(AAsset *asset, off_t *outStart, off_t *outLength)
{
    if (!asset)
        return -1;

    if (asset->tmp_fd < 0) {
        char tmpl[] = "/tmp/deadspace_asset_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0)
            return -1;

        size_t written = 0;
        while (written < asset->size) {
            ssize_t n = write(fd, asset->data + written, asset->size - written);
            if (n <= 0) {
                close(fd);
                unlink(tmpl);
                return -1;
            }
            written += (size_t)n;
        }
        lseek(fd, 0, SEEK_SET);

        asset->tmp_fd   = fd;
        asset->tmp_path = strdup(tmpl);
        /* Unlink now: the fd keeps it alive and nothing leaks on exit. */
        unlink(tmpl);
    }

    if (outStart)  *outStart  = 0;
    if (outLength) *outLength = (off_t)asset->size;
    return dup(asset->tmp_fd);
}

extern "C" void AAsset_close(AAsset *asset)
{
    if (!asset)
        return;
    if (asset->tmp_fd >= 0)
        close(asset->tmp_fd);
    free(asset->tmp_path);
    free(asset->data);
    free(asset);
}

/* ------------------------------------------------------------------ *
 * AAssetDir — the game walks assets/Pods/ and assets/Patterns/ to discover
 * content, so directory listing has to work, not just direct opens.
 * ------------------------------------------------------------------ */
extern "C" AAssetDir *AAssetManager_openDir(AAssetManager *mgr, const char *dirName)
{
    (void)mgr;
    if (!g_apk)
        return NULL;

    std::string prefix = ASSET_PREFIX;
    if (dirName && *dirName) {
        prefix += dirName;
        if (prefix[prefix.size() - 1] != '/')
            prefix += '/';
    }

    AAssetDir *dir = new AAssetDir();
    dir->next = 0;

    zip_int64_t n = zip_get_num_entries(g_apk, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char *name = zip_get_name(g_apk, i, 0);
        if (!name || strncmp(name, prefix.c_str(), prefix.size()) != 0)
            continue;

        const char *rest = name + prefix.size();
        if (!*rest || strchr(rest, '/'))
            continue;   /* NDK semantics: files only, no recursion */

        dir->entries.push_back(rest);
    }

    return dir;
}

extern "C" const char *AAssetDir_getNextFileName(AAssetDir *dir)
{
    if (!dir || dir->next >= dir->entries.size())
        return NULL;
    return dir->entries[dir->next++].c_str();
}

extern "C" void AAssetDir_close(AAssetDir *dir)
{
    delete dir;
}

/* ------------------------------------------------------------------ *
 * AConfiguration — the game reads it only for language/country.
 * ------------------------------------------------------------------ */
struct AConfiguration {
    char language[2];
    char country[2];
};

extern "C" AConfiguration *AConfiguration_new(void)
{
    AConfiguration *c = (AConfiguration *)calloc(1, sizeof(AConfiguration));
    if (c) {
        c->language[0] = 'e'; c->language[1] = 'n';
        c->country[0]  = 'U'; c->country[1]  = 'S';
    }
    return c;
}

extern "C" void AConfiguration_delete(AConfiguration *config)
{
    free(config);
}

extern "C" void AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *am)
{
    (void)am;
    if (!out)
        return;

    /* Derive from the host locale so the game picks the right strings. */
    const char *lang = getenv("LANG");
    if (lang && strlen(lang) >= 5 && lang[2] == '_') {
        out->language[0] = lang[0];
        out->language[1] = lang[1];
        out->country[0]  = lang[3];
        out->country[1]  = lang[4];
    }
}

extern "C" void AConfiguration_getLanguage(AConfiguration *config, char *outLanguage)
{
    if (!config || !outLanguage)
        return;
    outLanguage[0] = config->language[0];
    outLanguage[1] = config->language[1];
    outLanguage[2] = 0;
}

extern "C" void AConfiguration_getCountry(AConfiguration *config, char *outCountry)
{
    if (!config || !outCountry)
        return;
    outCountry[0] = config->country[0];
    outCountry[1] = config->country[1];
    outCountry[2] = 0;
}
