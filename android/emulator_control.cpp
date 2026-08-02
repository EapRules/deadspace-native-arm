/*
 * File-backed control channel for the local qemu emulator.
 *
 * A host process appends one command per line to <control>/commands. The
 * loader consumes at most one line per game frame, which guarantees that a
 * down/up pair spans frames even if the MCP writes both immediately.
 *
 * Supported commands:
 *   cursor X Y
 *   click down|up
 *   button NAME down|up
 *   stick left|right X Y
 *   screenshot TOKEN
 *   quit
 *
 * Screenshots are captured from the real default framebuffer and written as
 * RGBA PNG files under <control>/screenshots/. No game asset crosses this
 * interface; it only exposes pixels already rendered by the user's own copy.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <sstream>
#include <vector>

#include <SDL2/SDL.h>

#include <algorithm>
#include <zlib.h>

#include "khronos/glad.h"
#include "trace.h"

#include "emulator_control.h"
#include "input_bridge.h"

static std::string g_control_dir;
static std::string g_command_path;
static FILE *g_commands = NULL;
static std::string g_screenshot_token;
static bool g_enabled = false;
static std::vector<long> g_auto_screenshot_frames;
static size_t g_auto_screenshot_index = 0;

static uintptr_t find_gles1_function(const char *name)
{
    return (uintptr_t)SDL_GL_GetProcAddress(name);
}

static bool safe_token(const char *token)
{
    if (!token || !*token)
        return false;
    size_t n = strlen(token);
    if (n > 80)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

static void write_status(long frame, const char *state,
                         const char *screenshot = NULL)
{
    if (!g_enabled)
        return;

    std::string temp = g_control_dir + "/status.json.tmp";
    std::string final = g_control_dir + "/status.json";
    FILE *out = fopen(temp.c_str(), "w");
    if (!out)
        return;
    fprintf(out, "{\"state\":\"%s\",\"frame\":%ld", state, frame);
    if (screenshot)
        fprintf(out, ",\"screenshot\":\"%s\"", screenshot);
    fprintf(out, "}\n");
    fclose(out);
    rename(temp.c_str(), final.c_str());
}

void emulator_control_init(void)
{
    const char *dir = getenv("DEADSPACE_CONTROL_DIR");
    if (!dir || !*dir)
        return;

    g_control_dir = dir;
    g_command_path = g_control_dir + "/commands";
    mkdir(g_control_dir.c_str(), 0775);
    std::string shots = g_control_dir + "/screenshots";
    mkdir(shots.c_str(), 0775);

    FILE *create = fopen(g_command_path.c_str(), "a");
    if (create)
        fclose(create);
    g_commands = fopen(g_command_path.c_str(), "r");
    const char *auto_frames = getenv("DEADSPACE_AUTO_SCREENSHOT_FRAMES");
    if (!auto_frames || !*auto_frames)
        auto_frames = getenv("DEADSPACE_AUTO_SCREENSHOT_FRAME");
    if (auto_frames && *auto_frames) {
        std::stringstream values(auto_frames);
        std::string value;
        while (std::getline(values, value, ',')) {
            long frame = atol(value.c_str());
            if (frame >= 0)
                g_auto_screenshot_frames.push_back(frame);
        }
        std::sort(g_auto_screenshot_frames.begin(),
                  g_auto_screenshot_frames.end());
    }
    if (!g_commands) {
        trace("emulator control disabled: cannot open %s: %s",
              g_command_path.c_str(), strerror(errno));
        return;
    }

    g_enabled = true;
    write_status(0, "ready");
    trace("emulator control ready at %s", g_control_dir.c_str());
}

bool emulator_control_tick(long frame)
{
    if (!g_enabled || !g_commands)
        return true;

    char line[256];
    if (!fgets(line, sizeof(line), g_commands)) {
        clearerr(g_commands);
        if (frame % 30 == 0)
            write_status(frame, "running");
        return true;
    }

    char action[32] = {};
    if (sscanf(line, "%31s", action) != 1)
        return true;

    if (!strcmp(action, "cursor")) {
        float x = 0, y = 0;
        if (sscanf(line, "%*s %f %f", &x, &y) == 2) {
            android_input_cursor_set(x, y);
            trace("emulator: cursor %.1f %.1f", (double)x, (double)y);
        }
    } else if (!strcmp(action, "click")) {
        char state[16] = {};
        if (sscanf(line, "%*s %15s", state) == 1)
            android_input_cursor_press(!strcmp(state, "down"));
    } else if (!strcmp(action, "button")) {
        char name[32] = {}, state[16] = {};
        if (sscanf(line, "%*s %31s %15s", name, state) == 2) {
            bool down = !strcmp(state, "down");
            if (!android_input_inject_control(name, down))
                trace("emulator: unknown control '%s'", name);
        }
    } else if (!strcmp(action, "stick")) {
        char name[16] = {};
        float x = 0.0f, y = 0.0f;
        if (sscanf(line, "%*s %15s %f %f", name, &x, &y) == 3) {
            if (android_input_inject_stick(name, x, y))
                trace("emulator: stick %s %.3f %.3f",
                      name, (double)x, (double)y);
            else
                trace("emulator: unknown stick '%s'", name);
        }
    } else if (!strcmp(action, "screenshot")) {
        char token[96] = {};
        if (sscanf(line, "%*s %95s", token) == 1 && safe_token(token))
            g_screenshot_token = token;
    } else if (!strcmp(action, "quit")) {
        trace("emulator: quit requested");
        return false;
    } else {
        trace("emulator: unknown command '%s'", action);
    }

    write_status(frame, "running");
    return true;
}

static void put_u32(FILE *out, uint32_t value)
{
    unsigned char bytes[4] = {
        (unsigned char)(value >> 24),
        (unsigned char)(value >> 16),
        (unsigned char)(value >> 8),
        (unsigned char)value,
    };
    fwrite(bytes, 1, sizeof(bytes), out);
}

static bool png_chunk(FILE *out, const char type[4],
                      const unsigned char *data, size_t size)
{
    if (size > 0xffffffffu)
        return false;
    put_u32(out, (uint32_t)size);
    fwrite(type, 1, 4, out);
    if (size)
        fwrite(data, 1, size, out);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (size)
        crc = crc32(crc, data, (uInt)size);
    put_u32(out, (uint32_t)crc);
    return ferror(out) == 0;
}

static bool write_png(const char *path, int width, int height,
                      const unsigned char *rgba)
{
    size_t stride = 1 + (size_t)width * 4;
    std::vector<unsigned char> raw(stride * (size_t)height);
    for (int y = 0; y < height; y++) {
        unsigned char *dst = raw.data() + (size_t)y * stride;
        const unsigned char *src =
            rgba + (size_t)(height - 1 - y) * (size_t)width * 4;
        dst[0] = 0;
        memcpy(dst + 1, src, (size_t)width * 4);
    }

    uLongf compressed_size = compressBound((uLong)raw.size());
    std::vector<unsigned char> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, raw.data(),
                  (uLong)raw.size(), Z_BEST_SPEED) != Z_OK)
        return false;
    compressed.resize(compressed_size);

    FILE *out = fopen(path, "wb");
    if (!out)
        return false;
    static const unsigned char signature[8] =
        {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(signature, 1, sizeof(signature), out);

    unsigned char ihdr[13] = {
        (unsigned char)(width >> 24), (unsigned char)(width >> 16),
        (unsigned char)(width >> 8), (unsigned char)width,
        (unsigned char)(height >> 24), (unsigned char)(height >> 16),
        (unsigned char)(height >> 8), (unsigned char)height,
        8, 6, 0, 0, 0,
    };
    bool ok = png_chunk(out, "IHDR", ihdr, sizeof(ihdr)) &&
              png_chunk(out, "IDAT", compressed.data(), compressed.size()) &&
              png_chunk(out, "IEND", NULL, 0);
    ok = ok && fclose(out) == 0;
    return ok;
}

void emulator_control_after_draw(long frame, int width, int height)
{
    if (g_enabled && g_screenshot_token.empty() &&
        g_auto_screenshot_index < g_auto_screenshot_frames.size() &&
        frame >= g_auto_screenshot_frames[g_auto_screenshot_index]) {
        long requested = g_auto_screenshot_frames[g_auto_screenshot_index++];
        g_screenshot_token = "auto-frame-" + std::to_string(requested);
    }
    if (!g_enabled || g_screenshot_token.empty() || width <= 0 || height <= 0)
        return;

    using ReadPixels = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                void *);
    using GetIntegerv = void (*)(GLenum, GLint *);
    using BindFramebuffer = void (*)(GLenum, GLuint);
    using PixelStorei = void (*)(GLenum, GLint);
    static ReadPixels read_pixels =
        (ReadPixels)find_gles1_function("glReadPixels");
    static GetIntegerv get_integer =
        (GetIntegerv)find_gles1_function("glGetIntegerv");
    static BindFramebuffer bind_framebuffer =
        (BindFramebuffer)find_gles1_function("glBindFramebufferOES");
    static PixelStorei pixel_store =
        (PixelStorei)find_gles1_function("glPixelStorei");

    if (!read_pixels || !get_integer || !bind_framebuffer || !pixel_store) {
        trace("emulator screenshot failed: GLES1 functions unresolved");
        g_screenshot_token.clear();
        return;
    }

    size_t size = (size_t)width * (size_t)height * 4;
    std::vector<unsigned char> pixels(size);
    GLint previous_fb = 0, previous_pack = 4;
    get_integer(GL_FRAMEBUFFER_BINDING, &previous_fb);
    get_integer(GL_PACK_ALIGNMENT, &previous_pack);
    bind_framebuffer(GL_FRAMEBUFFER, 0);
    pixel_store(GL_PACK_ALIGNMENT, 1);
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    pixel_store(GL_PACK_ALIGNMENT, previous_pack);
    bind_framebuffer(GL_FRAMEBUFFER, (GLuint)previous_fb);

    std::string relative = "screenshots/" + g_screenshot_token + ".png";
    std::string final = g_control_dir + "/" + relative;
    std::string temp = final + ".tmp";
    bool ok = write_png(temp.c_str(), width, height, pixels.data()) &&
              rename(temp.c_str(), final.c_str()) == 0;
    if (ok) {
        trace("emulator: screenshot %s at frame %ld",
              relative.c_str(), frame);
        write_status(frame, "running", relative.c_str());
    } else {
        trace("emulator screenshot failed: %s", strerror(errno));
        unlink(temp.c_str());
    }
    g_screenshot_token.clear();
}

void emulator_control_shutdown(long frame)
{
    if (!g_enabled)
        return;
    write_status(frame, "stopped");
    if (g_commands)
        fclose(g_commands);
    g_commands = NULL;
    g_enabled = false;
}
