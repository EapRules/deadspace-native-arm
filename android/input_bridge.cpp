/*
 * SDL controller input -> the JNI entry points exported by Dead Space.
 *
 * This game is a Java-driven Android application, not NativeActivity. It has
 * no AInputQueue/ALooper imports; its Android layer calls
 * KeyboardAndroid.NativeOnKey* and TouchSurfaceAndroid.NativeOnPointerEvent.
 * Feeding the native_app_glue queue therefore looks plausible but is dead
 * code for this binary. The working Vita port calls these same three exports,
 * with the signatures and module IDs copied below.
 *
 * Digital mapping follows that reference exactly. The sticks reproduce its
 * virtual touchscreen/touchpad positions, scaled from the Vita surface to the
 * actual 640x480 surface reported by this loader.
 *
 * DEADSPACE_AUTOPILOT is separate from normal controls. It presses a varied
 * sequence of keys after startup and samples a framebuffer strip before each
 * swap. A scene is counted only when a substantial visual change occurs in a
 * short window after a synthetic key, rather than counting animation as proof
 * that input worked.
 */
#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "jni.h"
#include "so_util.h"
#include "trace.h"

#include "input_bridge.h"

enum {
    AKEYCODE_BACK          = 4,
    AKEYCODE_DPAD_UP       = 19,
    AKEYCODE_DPAD_DOWN     = 20,
    AKEYCODE_DPAD_LEFT     = 21,
    AKEYCODE_DPAD_RIGHT    = 22,
    AKEYCODE_DPAD_CENTER   = 23,
    AKEYCODE_BUTTON_X      = 99,
    AKEYCODE_BUTTON_Y      = 100,
    AKEYCODE_BUTTON_L1     = 102,
    AKEYCODE_BUTTON_R1     = 103,
    AKEYCODE_BUTTON_START  = 108,
    AKEYCODE_BUTTON_SELECT = 109,
};

enum {
    ID_RAW_POINTER_DOWN = 0x6000c,
    ID_RAW_POINTER_MOVE = 0x4000c,
    ID_RAW_POINTER_UP   = 0x8000c,
    MODULE_KEYBOARD     = 600,
    MODULE_TOUCH_SCREEN = 1000,
    MODULE_TOUCH_PAD    = 1100,
};

using KeyFn = void (*)(JNIEnv *, void *, int, int, int);
/*
 * The game is armeabi softfp while this loader is armhf. pcs("aapcs") forces
 * the two float coordinates through core registers, which is the callee's ABI;
 * a normal hardfp function pointer would put them in s0/s1 and corrupt input.
 */
using PointerFn = void (__attribute__((pcs("aapcs"))) *)(
    JNIEnv *, void *, int, int, int, float, float);
using AccelFn = void (__attribute__((pcs("aapcs"))) *)(
    JNIEnv *, void *, float, float, float);

static JNIEnv    *g_env = NULL;
static KeyFn      g_key_down = NULL;
static KeyFn      g_key_up = NULL;
static PointerFn  g_pointer = NULL;
static AccelFn    g_acceleration = NULL;
static int        g_width = 640;
static int        g_height = 480;
static SDL_GameController *g_controllers[4] = {};

static bool g_left_active = false;
static bool g_right_active = false;
static int16_t g_lx = 0, g_ly = 0, g_rx = 0, g_ry = 0;
static float g_right_last_x = 0.0f, g_right_last_y = 0.0f;
static unsigned int g_right_pulses = 0;
static Uint8 g_accept_button = SDL_CONTROLLER_BUTTON_B;
static Uint8 g_back_button = SDL_CONTROLLER_BUTTON_A;

/*
 * The Xperia Play build uses accelerometer gestures for two actions that have
 * no ordinary key equivalent: tilting rotates/switches a weapon's fire mode,
 * and a sharp movement performs a Zero-G jump. R36S-class handhelds have no
 * motion sensor, while both analogue triggers are otherwise unused by this
 * JNI input path.
 *
 * These are the measured Android acceleration samples used by deadspace-vita's
 * optional D-pad accelerometer replacement, split at its explicit "for zero g
 * jump" boundary. Preserve the short cadence: the game recognises a gesture,
 * not a single static vector.
 */
struct AccelSample {
    float x;
    float y;
    float z;
    Uint16 delay_ms;
};

static const AccelSample kWeaponTilt[] = {
    {-0.998903f, -0.244644f, -1.292267f, 8},
    {-0.954221f, -0.367070f, -1.234914f, 8},
    {-1.029063f, -0.042471f, -1.256281f, 8},
    {-0.983264f, -0.268230f, -1.131455f, 8},
    {-1.049170f, -0.140187f, -1.296765f, 8},
    {-1.113959f, -0.304173f, -1.339498f, 8},
    {-1.655727f,  2.366763f, -2.261636f, 8},
    { 4.224865f, -2.941413f, -0.447724f, 8},
    { 2.926493f, -1.684568f, -1.615015f, 8},
    {-1.694824f,  1.514265f, -1.591399f, 8},
    {-2.287976f,  0.611223f, -1.149448f, 8},
    {-1.193269f, -0.221057f, -0.920038f, 8},
    {-1.024595f, -0.370441f, -1.369861f, 8},
    {-1.054755f, -0.157035f, -1.234914f, 8},
    {-1.159758f, -0.240151f, -1.291142f, 8},
    {-0.938582f, -0.179499f, -1.225918f, 8},
    {-1.046936f, -0.228920f, -1.210174f, 8},
    {-1.001137f, -0.234535f, -1.270900f, 8},
    {-1.055872f, -0.152542f, -1.367612f, 8},
    {-0.945284f, -0.057073f, -1.139327f, 8},
    {-1.002254f, -0.198594f, -1.224793f, 8},
    {-0.747567f, -0.235659f, -1.256281f, 8},
    {-0.699534f, -0.044716f, -1.474445f, 8},
    {-0.711821f,  0.117022f, -1.409221f, 8},
    {-1.023478f,  0.222602f, -1.628510f, 8},
    {-0.848101f, -0.006528f, -2.083956f, 8},
    {-0.272822f, -0.489498f, -3.042079f, 8},
    {-0.527508f, -0.292941f, -3.173653f, 8},
    {-1.081564f,  0.283254f, -3.189397f, 8},
    {-1.405508f,  0.348398f, -2.807047f, 8},
    {-1.814348f,  0.492166f, -2.401081f, 8},
    {-1.502691f,  0.095681f, -2.793552f, 8},
    {-1.847859f,  0.522491f, -3.261368f, 8},
};

static const AccelSample kZeroGJump[] = {
    {-1.761846f, -0.322144f, -1.657748f, 25},
    {-1.188801f, -0.022254f, -1.695983f, 8},
    {-0.794483f, -0.263738f, -2.931873f, 8},
    {-0.745333f, -0.150296f, -2.825040f, 8},
    {-0.873793f, -0.444571f, -1.743215f, 8},
    {-0.804536f, -0.127832f, -3.795533f, 8},
    {-1.742857f,  0.038399f, -1.233790f, 8},
    {-0.812356f,  0.224848f, -2.738449f, 8},
    { 0.863030f,  3.779731f, -9.721955f, 8},
    {-4.183607f,  3.622485f,  8.522037f, 8},
    {-6.169717f,  7.627765f, 23.052782f, 8},
    {-0.738630f,  6.358565f, 14.906132f, 8},
    { 0.151609f,  2.630712f,  9.449697f, 8},
    { 0.831056f,  3.416942f,  6.462183f, 8},
    {-2.905704f,  5.808204f, -9.787180f, 8},
    {-2.807403f,  1.275026f, -4.152019f, 8},
    {-2.541546f, -0.363701f,  0.349680f, 8},
    {-2.520322f,  0.236080f, -2.166049f, 8},
    {-1.934989f, -0.196346f, -0.763724f, 8},
    {-2.559419f, -0.453556f,  0.108265f, 8},
    {-1.904829f, -0.004282f, -1.038117f, 8},
    {-2.014299f, -0.291818f, -0.051879f, 8},
    {-2.021002f, -0.269354f, -1.045988f, 8},
    {-1.804294f, -0.230042f, -1.018999f, 8},
    {-2.016533f, -0.192978f, -0.518571f, 8},
    {-1.768549f, -0.101999f, -1.323754f, 8},
    {-1.541788f, -0.406383f, -1.025746f, 8},
};

static const AccelSample *g_accel_samples = NULL;
static size_t g_accel_count = 0;
static size_t g_accel_index = 0;
static Uint32 g_accel_next_ms = 0;
static const char *g_accel_name = NULL;
static bool g_l2_down = false;
static bool g_r2_down = false;

static void start_accel_gesture(const AccelSample *samples, size_t count,
                                const char *name)
{
    if (!g_acceleration) {
        warning("input: cannot start %s gesture; acceleration JNI symbol is missing\n",
                name);
        return;
    }
    g_accel_samples = samples;
    g_accel_count = count;
    g_accel_index = 0;
    g_accel_next_ms = SDL_GetTicks();
    g_accel_name = name;
    trace("input: %s accelerometer gesture started (%zu samples)", name, count);
}

static void update_accel_gesture(void)
{
    if (!g_accel_samples || !g_acceleration)
        return;

    Uint32 now = SDL_GetTicks();
    while (g_accel_index < g_accel_count &&
           (Sint32)(now - g_accel_next_ms) >= 0) {
        const AccelSample &sample = g_accel_samples[g_accel_index++];
        g_acceleration(g_env, (void *)0x42424242,
                       sample.x, sample.y, sample.z);
        g_accel_next_ms += sample.delay_ms;
    }

    if (g_accel_index == g_accel_count) {
        trace("input: %s accelerometer gesture completed", g_accel_name);
        g_accel_samples = NULL;
        g_accel_count = 0;
        g_accel_index = 0;
        g_accel_name = NULL;
    }
}

static void trigger_changed(bool left, Sint16 value)
{
    bool down = value > 16000;
    bool &was_down = left ? g_l2_down : g_r2_down;
    if (down && !was_down) {
        if (left)
            start_accel_gesture(kWeaponTilt,
                                sizeof(kWeaponTilt) / sizeof(kWeaponTilt[0]),
                                "weapon-tilt/L2");
        else
            start_accel_gesture(kZeroGJump,
                                sizeof(kZeroGJump) / sizeof(kZeroGJump[0]),
                                "zero-g/R2");
    }
    was_down = down;
}

/*
 * Dead Space's menus are touch-only. The Vita port uses the front touchscreen
 * and explicitly documents that a pad cannot navigate them. R36S has no touch
 * panel, so the d-pad drives this software cursor and physical A taps it.
 *
 * The cursor starts visible for the title/menu. Moving either analog stick
 * means gameplay and dismisses it; L3 brings it back when a menu is opened.
 */
static const int kCursorPointerId = 3;
static const float kCursorSpeed = 420.0f;
static float g_cursor_x = -1.0f, g_cursor_y = -1.0f;
static int g_cursor_dx = 0, g_cursor_dy = 0;
static bool g_cursor_visible = false;
static bool g_cursor_down = false;
static Uint32 g_cursor_last_ms = 0;

static bool g_autopilot = false;
static bool g_autopilot_fast = false;
static bool g_autopilot_visual = false;
static long g_auto_keys = 0;
static long g_auto_scenes = 0;
static long g_pending_key_frame = -1;
static int  g_pending_key_serial = 0;
static int  g_counted_key_serial = 0;

static void update_sticks(void);

static const int kAutopilotKeys[] = {
    AKEYCODE_DPAD_CENTER,
    AKEYCODE_DPAD_DOWN,
    AKEYCODE_DPAD_CENTER,
    AKEYCODE_BUTTON_START,
    AKEYCODE_BACK,
    AKEYCODE_DPAD_RIGHT,
    AKEYCODE_DPAD_CENTER,
    AKEYCODE_DPAD_UP,
};

static int map_button(Uint8 button)
{
    if (button == g_accept_button)
        return AKEYCODE_DPAD_CENTER;
    if (button == g_back_button)
        return AKEYCODE_BACK;

    switch (button) {
    case SDL_CONTROLLER_BUTTON_X:             return AKEYCODE_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y:             return AKEYCODE_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return AKEYCODE_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AKEYCODE_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_START:         return AKEYCODE_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_BACK:          return AKEYCODE_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return AKEYCODE_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return AKEYCODE_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return AKEYCODE_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return AKEYCODE_DPAD_RIGHT;
    default:                                  return 0;
    }
}

static void send_key(int code, bool down)
{
    KeyFn fn = down ? g_key_down : g_key_up;
    if (fn)
        fn(g_env, (void *)0x42424242, MODULE_KEYBOARD, code, 1);
}

static float normalise_axis(int16_t value, float deadzone, float maximum)
{
    float v = value / 32767.0f;
    float magnitude = fabsf(v);
    if (magnitude <= deadzone)
        return 0.0f;
    float scaled = (magnitude - deadzone) / (1.0f - deadzone);
    scaled = std::min(scaled, maximum);
    return copysignf(scaled, v);
}

static void send_pointer(int raw_event, int module, int id, float x, float y)
{
    if (g_pointer)
        g_pointer(g_env, (void *)0x42424242, raw_event, module, id, x, y);
}

static void cursor_show(void)
{
    if (!g_cursor_visible) {
        g_cursor_x = g_width * 0.5f;
        g_cursor_y = g_height * 0.5f;
        trace("input: menu cursor shown at %.0f,%.0f", g_cursor_x, g_cursor_y);
    }
    g_cursor_visible = true;
    g_cursor_last_ms = SDL_GetTicks();
}

static void cursor_hide(void)
{
    if (g_cursor_down) {
        send_pointer(ID_RAW_POINTER_UP, MODULE_TOUCH_SCREEN, kCursorPointerId,
                     g_cursor_x, g_cursor_y);
        g_cursor_down = false;
    }
    if (g_cursor_visible)
        trace("input: menu cursor hidden");
    g_cursor_visible = false;
    g_cursor_dx = 0;
    g_cursor_dy = 0;
}

static void cursor_tap(bool down)
{
    if (!g_cursor_visible)
        return;
    send_pointer(down ? ID_RAW_POINTER_DOWN : ID_RAW_POINTER_UP,
                 MODULE_TOUCH_SCREEN, kCursorPointerId,
                 g_cursor_x, g_cursor_y);
    g_cursor_down = down;
    trace("input: menu tap %s at %.0f,%.0f",
          down ? "down" : "up", g_cursor_x, g_cursor_y);
}

void android_input_tick(void)
{
    /*
     * SDL axis events report value changes, not elapsed held time. The game
     * consumes both virtual sticks once per Android poll, so refresh them once
     * per rendered frame even when the physical axis value is unchanged.
     */
    update_sticks();
    update_accel_gesture();

    Uint32 now = SDL_GetTicks();
    if (!g_cursor_last_ms)
        g_cursor_last_ms = now;
    float dt = (now - g_cursor_last_ms) / 1000.0f;
    g_cursor_last_ms = now;

    if (!g_cursor_visible || (!g_cursor_dx && !g_cursor_dy))
        return;

    g_cursor_x += g_cursor_dx * kCursorSpeed * dt;
    g_cursor_y += g_cursor_dy * kCursorSpeed * dt;
    g_cursor_x = std::max(0.0f, std::min(g_cursor_x, (float)g_width - 1.0f));
    g_cursor_y = std::max(0.0f, std::min(g_cursor_y, (float)g_height - 1.0f));

    if (g_cursor_down)
        send_pointer(ID_RAW_POINTER_MOVE, MODULE_TOUCH_SCREEN, kCursorPointerId,
                     g_cursor_x, g_cursor_y);
}

extern "C" void android_input_cursor_position(float *x, float *y, int *visible)
{
    if (x)
        *x = g_cursor_x;
    if (y)
        *y = g_cursor_y;
    if (visible)
        *visible = g_cursor_visible ? 1 : 0;
}

void android_input_cursor_set(float x, float y)
{
    cursor_show();
    g_cursor_x = std::max(0.0f, std::min(x, (float)g_width - 1.0f));
    g_cursor_y = std::max(0.0f, std::min(y, (float)g_height - 1.0f));
}

void android_input_cursor_press(bool down)
{
    cursor_tap(down);
}

bool android_input_inject_control(const char *name, bool down)
{
    if (!name)
        return false;

    if (!strcasecmp(name, "l2") || !strcasecmp(name, "r2")) {
        SDL_Event event = {};
        event.type = SDL_CONTROLLERAXISMOTION;
        event.caxis.axis = !strcasecmp(name, "l2")
            ? SDL_CONTROLLER_AXIS_TRIGGERLEFT
            : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        event.caxis.value = down ? 32767 : 0;
        return android_input_event(&event);
    }

    Uint8 button;
    if (!strcasecmp(name, "a"))             button = g_accept_button;
    else if (!strcasecmp(name, "b"))        button = g_back_button;
    else if (!strcasecmp(name, "x"))        button = SDL_CONTROLLER_BUTTON_X;
    else if (!strcasecmp(name, "y"))        button = SDL_CONTROLLER_BUTTON_Y;
    else if (!strcasecmp(name, "l1"))       button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    else if (!strcasecmp(name, "r1"))       button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    else if (!strcasecmp(name, "start"))    button = SDL_CONTROLLER_BUTTON_START;
    else if (!strcasecmp(name, "select"))   button = SDL_CONTROLLER_BUTTON_BACK;
    else if (!strcasecmp(name, "l3"))       button = SDL_CONTROLLER_BUTTON_LEFTSTICK;
    else if (!strcasecmp(name, "r3"))       button = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    else if (!strcasecmp(name, "up"))       button = SDL_CONTROLLER_BUTTON_DPAD_UP;
    else if (!strcasecmp(name, "down"))     button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    else if (!strcasecmp(name, "left"))     button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    else if (!strcasecmp(name, "right"))    button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    else return false;

    SDL_Event event = {};
    event.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    event.cbutton.button = button;
    return android_input_event(&event);
}

bool android_input_inject_stick(const char *name, float x, float y)
{
    if (!name || (strcasecmp(name, "left") && strcasecmp(name, "right")))
        return false;

    x = std::max(-1.0f, std::min(x, 1.0f));
    y = std::max(-1.0f, std::min(y, 1.0f));
    int16_t raw_x = (int16_t)lrintf(x * 32767.0f);
    int16_t raw_y = (int16_t)lrintf(y * 32767.0f);
    if (!strcasecmp(name, "left")) {
        g_lx = raw_x;
        g_ly = raw_y;
    } else {
        g_rx = raw_x;
        g_ry = raw_y;
    }
    return true;
}

static void update_sticks(void)
{
    if (!g_pointer)
        return;

    float lx = normalise_axis(g_lx, 0.15f, 0.76f);
    float ly = normalise_axis(g_ly, 0.15f, 0.76f);
    float rx = normalise_axis(g_rx, 0.15f, 1.00f);
    float ry = normalise_axis(g_ry, 0.15f, 1.00f);

    const float left_base_x  = g_width  * (192.0f / 960.0f);
    const float left_base_y  = g_height * ( 75.0f / 544.0f);
    const float right_base_x = g_width  * (786.0f / 960.0f);
    const float right_base_y = g_height * (180.0f / 544.0f);

    float left_x  = left_base_x  + g_width  * ( 75.0f / 960.0f) * lx;
    float left_y  = left_base_y  + g_height * ( 75.0f / 544.0f) * ly;
    float right_x = right_base_x + g_width  * (155.0f / 960.0f) * rx;
    float right_y = right_base_y - g_height * (105.0f / 544.0f) * ry;

    bool left_now = lx != 0.0f || ly != 0.0f;
    bool right_now = rx != 0.0f || ry != 0.0f;
    if (left_now || right_now)
        cursor_hide();

    if (left_now && !g_left_active)
        send_pointer(ID_RAW_POINTER_DOWN, MODULE_TOUCH_SCREEN, 1,
                     left_base_x, left_base_y);
    if (left_now)
        send_pointer(ID_RAW_POINTER_MOVE, MODULE_TOUCH_SCREEN, 1,
                     left_x, left_y);
    else if (g_left_active)
        send_pointer(ID_RAW_POINTER_UP, MODULE_TOUCH_SCREEN, 1,
                     left_x, left_y);
    g_left_active = left_now;

    /*
     * The original Vita bridge ends and restarts the right-stick swipe on
     * every poll. Dead Space treats one long touchpad drag as a finite camera
     * gesture; repeatedly pulsing UP/DOWN/MOVE makes a held stick rotate
     * continuously. Keep the left movement stick as one sustained drag.
     */
    if (g_right_active)
        send_pointer(ID_RAW_POINTER_UP, MODULE_TOUCH_PAD, 2,
                     g_right_last_x, g_right_last_y);
    if (right_now) {
        if (!g_right_active) {
            g_right_pulses = 0;
            trace("input: right-stick camera gesture started");
        }
        send_pointer(ID_RAW_POINTER_DOWN, MODULE_TOUCH_PAD, 2,
                     right_base_x, right_base_y);
        send_pointer(ID_RAW_POINTER_MOVE, MODULE_TOUCH_PAD, 2,
                     right_x, right_y);
        g_right_last_x = right_x;
        g_right_last_y = right_y;
        g_right_pulses++;
        if (g_right_pulses == 30)
            trace("input: right-stick camera gesture remains continuous "
                  "(30 frame pulses)");
    } else if (g_right_active) {
        trace("input: right-stick camera gesture ended after %u frame pulses",
              g_right_pulses);
    }
    g_right_active = right_now;
}

static void open_controller(int index)
{
    if (!SDL_IsGameController(index))
        return;
    for (SDL_GameController *controller : g_controllers)
        if (controller &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) ==
                SDL_JoystickGetDeviceInstanceID(index))
            return;
    for (SDL_GameController *&slot : g_controllers) {
        if (!slot) {
            slot = SDL_GameControllerOpen(index);
            if (slot)
                trace("controller: %s", SDL_GameControllerName(slot));
            return;
        }
    }
}

void android_input_init(so_module *mod, JNIEnv *env, int width, int height)
{
    g_env = env;
    g_width = width;
    g_height = height;
    g_key_down = (KeyFn)so_symbol(
        mod, "Java_com_ea_blast_KeyboardAndroid_NativeOnKeyDown");
    g_key_up = (KeyFn)so_symbol(
        mod, "Java_com_ea_blast_KeyboardAndroid_NativeOnKeyUp");
    g_pointer = (PointerFn)so_symbol(
        mod, "Java_com_ea_blast_TouchSurfaceAndroid_NativeOnPointerEvent");
    g_acceleration = (AccelFn)so_symbol(
        mod, "Java_com_ea_blast_AccelerometerAndroidDelegate_NativeOnAcceleration");

    const char *auto_env = getenv("DEADSPACE_AUTOPILOT");
    g_autopilot = auto_env && *auto_env && strcmp(auto_env, "0") != 0;
    const char *fast_env = getenv("DEADSPACE_AUTOPILOT_FAST");
    g_autopilot_fast = fast_env && *fast_env && strcmp(fast_env, "0") != 0;
    const char *visual_env = getenv("DEADSPACE_AUTOPILOT_VISUAL");
    g_autopilot_visual = visual_env && *visual_env && strcmp(visual_env, "0") != 0;

    /*
     * SDL names face buttons by position. R36S-class handhelds are lettered
     * Nintendo-style: their physical A (right) arrives as SDL B, while their
     * physical B (bottom) arrives as SDL A. This is measured on the same
     * device by the working sibling ports.
     */
    const char *layout = getenv("DEADSPACE_FACE_LAYOUT");
    if (layout && strcmp(layout, "xbox") == 0) {
        g_accept_button = SDL_CONTROLLER_BUTTON_A;
        g_back_button = SDL_CONTROLLER_BUTTON_B;
    }

    int available = SDL_NumJoysticks();
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        open_controller(i);
    }
    int opened = 0;
    for (SDL_GameController *controller : g_controllers)
        if (controller)
            opened++;

    trace("input bridge: JNI keys=%s pointer=%s acceleration=%s "
          "joysticks=%d controllers=%d "
          "face-layout=%s%s",
          g_key_down && g_key_up ? "yes" : "no", g_pointer ? "yes" : "no",
          g_acceleration ? "yes" : "no",
          available, opened,
          g_accept_button == SDL_CONTROLLER_BUTTON_B ? "Nintendo" : "Xbox",
          g_autopilot ? (g_autopilot_visual ? " autopilot=visual" :
                         (g_autopilot_fast ? " autopilot=fast" : " autopilot=on")) : "");

    cursor_show();
    trace("input: menus use d-pad cursor + physical A tap; L3/R3 toggle cursor; "
          "Start restores it");
    trace("input: L2 simulates weapon tilt; R2 simulates Zero-G motion");
}

bool android_input_event(const SDL_Event *event)
{
    if (!event)
        return true;

    switch (event->type) {
    case SDL_QUIT:
        return false;
    case SDL_CONTROLLERDEVICEADDED:
        open_controller(event->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        for (SDL_GameController *&slot : g_controllers) {
            if (!slot)
                continue;
            SDL_Joystick *stick = SDL_GameControllerGetJoystick(slot);
            if (SDL_JoystickInstanceID(stick) == event->cdevice.which) {
                SDL_GameControllerClose(slot);
                slot = NULL;
            }
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        bool down = event->type == SDL_CONTROLLERBUTTONDOWN;
        Uint8 button = event->cbutton.button;

        if (button == SDL_CONTROLLER_BUTTON_LEFTSTICK ||
            button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
            if (down) {
                if (g_cursor_visible)
                    cursor_hide();
                else
                    cursor_show();
            }
            trace("input: controller button=%u %s -> menu cursor toggle",
                  (unsigned int)button, down ? "down" : "up");
            break;
        }

        /*
         * Analog movement deliberately hides the menu cursor for gameplay.
         * Opening the pause menu is the reliable recovery path even on devices
         * whose stick-click buttons are not exposed by their SDL mapping.
         */
        if (button == SDL_CONTROLLER_BUTTON_START && down &&
            !g_cursor_visible)
            cursor_show();

        if (g_cursor_visible) {
            switch (button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                g_cursor_dx = down ? -1 : (g_cursor_dx < 0 ? 0 : g_cursor_dx);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                g_cursor_dx = down ? 1 : (g_cursor_dx > 0 ? 0 : g_cursor_dx);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                g_cursor_dy = down ? -1 : (g_cursor_dy < 0 ? 0 : g_cursor_dy);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                g_cursor_dy = down ? 1 : (g_cursor_dy > 0 ? 0 : g_cursor_dy);
                break;
            default:
                break;
            }
            if (button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
                button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                trace("input: controller button=%u %s -> menu cursor",
                      (unsigned int)button, down ? "down" : "up");
                break;
            }
            if (button == g_accept_button) {
                cursor_tap(down);
                break;
            }
        }

        int code = map_button(event->cbutton.button);
        trace("input: controller button=%u %s -> Android key=%d",
              (unsigned int)event->cbutton.button,
              down ? "down" : "up", code);
        if (code)
            send_key(code, down);
        break;
    }
    case SDL_CONTROLLERAXISMOTION:
        {
        static int axis_lines = 0;
        if (axis_lines < 24 && abs((int)event->caxis.value) > 4000) {
            trace("input: controller axis=%u value=%d",
                  (unsigned int)event->caxis.axis,
                  (int)event->caxis.value);
            axis_lines++;
        }
        switch (event->caxis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:  g_lx = event->caxis.value; break;
        case SDL_CONTROLLER_AXIS_LEFTY:  g_ly = event->caxis.value; break;
        case SDL_CONTROLLER_AXIS_RIGHTX: g_rx = event->caxis.value; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: g_ry = event->caxis.value; break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            trigger_changed(true, event->caxis.value);
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            trigger_changed(false, event->caxis.value);
            break;
        default: return true;
        }
        break;
        }
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (event->key.keysym.sym == SDLK_ESCAPE)
            send_key(AKEYCODE_BACK, event->type == SDL_KEYDOWN);
        else if (event->key.keysym.sym == SDLK_RETURN)
            send_key(AKEYCODE_DPAD_CENTER, event->type == SDL_KEYDOWN);
        break;
    default:
        break;
    }
    return true;
}

void android_input_autopilot_tick(long frame)
{
    if (!g_autopilot)
        return;

    /* The title screen is touch-only.  A center-key press is not equivalent
     * to tapping the visible Play button, so drive the software cursor first
     * and then continue with the physical-key script for the following menus. */
    if (g_pointer && frame == 90) {
        g_cursor_visible = true;
        g_cursor_x = g_width * 0.15f;
        g_cursor_y = g_height * 0.89f;
        cursor_tap(true);
    } else if (g_pointer && frame == 93) {
        g_cursor_visible = true;
        cursor_tap(false);
        trace("autopilot: title Play tap completed");
    } else if (g_pointer && frame == 180) {
        g_cursor_visible = true;
        g_cursor_x = g_width * 0.50f;
        g_cursor_y = g_height * 0.62f;
        cursor_tap(true);
    } else if (g_pointer && frame == 183) {
        cursor_tap(false);
        trace("autopilot: save-slot tap completed");
    } else if (g_pointer && frame == 270) {
        g_cursor_visible = true;
        g_cursor_x = g_width * 0.50f;
        g_cursor_y = g_height * 0.80f;
        cursor_tap(true);
    } else if (g_pointer && frame == 273) {
        cursor_tap(false);
        trace("autopilot: confirm tap completed");
    } else if (g_pointer && frame >= 900 && frame <= 1800 &&
               ((frame - 900) % 300 == 0)) {
        /* Difficulty screen: activate the bottom-right checkmark after the
         * clean-install splash/title sequence has settled.  A fixture save
         * does not skip those startup movies; measured entry is ~2310. */
        g_cursor_visible = true;
        g_cursor_x = g_width * 0.86f;
        g_cursor_y = g_height * 0.93f;
        cursor_tap(true);
    } else if (g_pointer && frame >= 903 && frame <= 1803 &&
               ((frame - 903) % 300 == 0)) {
        cursor_tap(false);
        trace("autopilot: difficulty checkmark tap completed");
    }

    /* The scripted gameplay keys periodically press Start to exercise the
     * pause/menu path.  Immediately release that menu again so screenshots
     * after the intro show the player model, rather than the pause overlay. */
    if (g_key_down && g_key_up && frame >= 2700 && frame <= 2880 &&
        ((frame - 2700) % 180 == 0)) {
        if (g_pointer) {
            g_cursor_visible = true;
            g_cursor_x = g_width * 0.24f;
            /* Movement Controls hand is near the upper-left quadrant. */
            g_cursor_y = g_height * 0.35f;
            cursor_tap(true);
        } else {
            send_key(AKEYCODE_BACK, true);
        }
    } else if (g_key_down && g_key_up && frame >= 2703 && frame <= 2883 &&
               ((frame - 2703) % 180 == 0)) {
        if (g_pointer)
            cursor_tap(false);
        else
            send_key(AKEYCODE_BACK, false);
        trace("autopilot: gameplay overlay tap completed at frame %ld", frame);
    }

    /* Movement Controls advances only after the game observes a sustained
     * analog gesture.  Complete that tutorial before visual sampling so its
     * bright arrows cannot make a black Isaac look like a healthy ROI. */
    if (frame == 2706) {
        g_lx = 24000;
        g_ly = 0;
        trace("autopilot: tutorial movement started");
    } else if (frame == 2820) {
        g_lx = 0;
        g_ly = 0;
        trace("autopilot: tutorial movement completed");
    }

    /* Once the first pause menu is dismissed, hold the scene steady for the
     * screenshot harness; continuing the menu-key loop would open Help or
     * another overlay before the player model can be inspected. */
    if (g_autopilot_visual || frame >= 2403)
        return;

    if (!g_key_down || !g_key_up)
        return;

    const long first = 90;
    const long period = g_autopilot_fast ? 8 : 45;
    if (frame < first)
        return;

    long phase = (frame - first) % period;
    long index = (frame - first) / period;
    int code = kAutopilotKeys[index %
        (sizeof(kAutopilotKeys) / sizeof(kAutopilotKeys[0]))];

    if (phase == 0) {
        send_key(code, true);
        g_auto_keys++;
        g_pending_key_frame = frame;
        g_pending_key_serial++;
        trace("autopilot: key down %d at frame %ld", code, frame);
    } else if (phase == 3) {
        send_key(code, false);
    }
}

/*
 * A compact perceptual signature: 32 horizontal buckets over the middle row,
 * RGB averaged in each. It is deliberately coarser than a pixel hash so menu
 * animation and tiny cursor movement do not become fake scene transitions.
 */
static bool sample_signature(unsigned char signature[32 * 3])
{
    using ReadPixels = void (*)(int, int, int, int, unsigned int, unsigned int,
                                void *);
    static ReadPixels read_pixels =
        (ReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    if (!read_pixels)
        return false;

    static unsigned char *row = NULL;
    static size_t row_size = 0;
    size_t need = (size_t)g_width * 4;
    if (need > row_size) {
        unsigned char *next = (unsigned char *)realloc(row, need);
        if (!next)
            return false;
        row = next;
        row_size = need;
    }

    read_pixels(0, g_height / 2, g_width, 1, 0x1908 /* GL_RGBA */,
                0x1401 /* GL_UNSIGNED_BYTE */, row);

    for (int bucket = 0; bucket < 32; bucket++) {
        int begin = bucket * g_width / 32;
        int end = (bucket + 1) * g_width / 32;
        unsigned int sum[3] = {};
        for (int x = begin; x < end; x++) {
            sum[0] += row[x * 4 + 0];
            sum[1] += row[x * 4 + 1];
            sum[2] += row[x * 4 + 2];
        }
        int pixels = std::max(1, end - begin);
        signature[bucket * 3 + 0] = (unsigned char)(sum[0] / pixels);
        signature[bucket * 3 + 1] = (unsigned char)(sum[1] / pixels);
        signature[bucket * 3 + 2] = (unsigned char)(sum[2] / pixels);
    }
    return true;
}

void android_input_autopilot_sample(long frame)
{
    if (!g_autopilot || frame % 15 != 0)
        return;

    static unsigned char latest[32 * 3] = {};
    static unsigned char baseline[32 * 3] = {};
    static bool have_latest = false;
    static int baseline_serial = 0;

    unsigned char current[32 * 3];
    if (!sample_signature(current))
        return;

    if (g_pending_key_serial != baseline_serial && have_latest) {
        memcpy(baseline, latest, sizeof(baseline));
        baseline_serial = g_pending_key_serial;
    }

    if (baseline_serial != 0 &&
        baseline_serial != g_counted_key_serial &&
        frame >= g_pending_key_frame + 10 &&
        frame <= g_pending_key_frame + 44) {
        long difference = 0;
        for (size_t i = 0; i < sizeof(current); i++)
            difference += abs((int)current[i] - (int)baseline[i]);
        long average = difference / (long)sizeof(current);
        if (average >= 18) {
            g_auto_scenes++;
            g_counted_key_serial = baseline_serial;
            trace("autopilot: scene change %ld after key %d "
                  "(mean strip delta=%ld)",
                  g_auto_scenes, baseline_serial, average);
        }
    }

    memcpy(latest, current, sizeof(latest));
    have_latest = true;
}

long android_input_autopilot_keys(void) { return g_auto_keys; }
long android_input_autopilot_scenes(void) { return g_auto_scenes; }
