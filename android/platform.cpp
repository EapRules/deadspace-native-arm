/*
 * ANativeWindow, ALooper and AInputQueue backed by SDL2.
 *
 * The game runs its own loop: it calls ALooper_pollAll(), drains AInputQueue,
 * and draws. So the job here is to pump SDL and translate its events into the
 * Android event objects the game already knows how to read.
 *
 * The OUYA build expects a gamepad and the Play build expects touch, so both
 * are produced: buttons/sticks become key events, and the right stick also
 * synthesises touch points for the Play build's twin-stick controls.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, for the button-layout override */
#include <deque>

#include <SDL2/SDL.h>

#include "android_api.h"
#include "app_exit.h"
#include "trace.h"

/* Android keycodes the game cares about (from android/keycodes.h). */
enum {
    AKEYCODE_BACK          = 4,
    AKEYCODE_DPAD_UP       = 19,
    AKEYCODE_MENU          = 82,
    AKEYCODE_DPAD_DOWN     = 20,
    AKEYCODE_DPAD_LEFT     = 21,
    AKEYCODE_DPAD_RIGHT    = 22,
    AKEYCODE_DPAD_CENTER   = 23,
    AKEYCODE_BUTTON_A      = 96,
    AKEYCODE_BUTTON_B      = 97,
    AKEYCODE_BUTTON_X      = 99,
    AKEYCODE_BUTTON_Y      = 100,
    AKEYCODE_BUTTON_L1     = 102,
    AKEYCODE_BUTTON_R1     = 103,
    AKEYCODE_BUTTON_L2     = 104,
    AKEYCODE_BUTTON_R2     = 105,
    AKEYCODE_BUTTON_START  = 108,
    AKEYCODE_BUTTON_SELECT = 109,
};

/* ------------------------------------------------------------------ *
 * The event object handed to the game. One struct serves both key and
 * motion events; the game picks fields via the AKeyEvent and AMotionEvent
 * accessors, so a tagged union is enough.
 * ------------------------------------------------------------------ */
#define MAX_POINTERS 4

/*
 * The axis numbers from android/input.h. They are not sequential with X and Y
 * because the list grew over several API levels, and the game indexes a table
 * by them, so the gaps have to be preserved.
 *
 * MAX_AXIS is 48 in Android; sizing the array to the highest one this port
 * ever fills keeps the event struct small, and getAxisValue() range-checks.
 */
enum {
    AMOTION_EVENT_AXIS_X        = 0,
    AMOTION_EVENT_AXIS_Y        = 1,
    AMOTION_EVENT_AXIS_Z        = 11,
    AMOTION_EVENT_AXIS_RZ       = 14,
    AMOTION_EVENT_AXIS_HAT_X    = 15,
    AMOTION_EVENT_AXIS_HAT_Y    = 16,
    AMOTION_EVENT_AXIS_LTRIGGER = 17,
    AMOTION_EVENT_AXIS_RTRIGGER = 18,
    AMOTION_EVENT_MAX_AXIS      = 19,
};

struct AInputEvent {
    int32_t type;
    int32_t source;
    int32_t deviceId;
    int32_t action;
    int32_t flags;
    int32_t metaState;
    /* key */
    int32_t keyCode;
    /* motion */
    size_t  pointerCount;
    int32_t pointerId[MAX_POINTERS];
    float   x[MAX_POINTERS];
    float   y[MAX_POINTERS];
    /* Every other axis, indexed by its AMOTION_EVENT_AXIS_* number. A real
     * joystick MotionEvent carries all of its axes at once, not one per
     * event, so they travel together here too. */
    float   axis[AMOTION_EVENT_MAX_AXIS];
};

struct AInputQueue {
    std::deque<AInputEvent *> pending;
};

static AInputQueue *g_queue   = NULL;
static SDL_Window  *g_window  = NULL;
static int          g_width   = 640;
static int          g_height  = 480;

static void face_layout_init(void);

extern "C" void android_platform_init(SDL_Window *win, int w, int h)
{
    g_window = win;
    g_width  = w;
    g_height = h;
    if (!g_queue)
        g_queue = new AInputQueue();
    face_layout_init();
}

/*
 * The handle the loader hands to onNativeWindowCreated. There is exactly one
 * surface — the SDL window — and every ANativeWindow_* entry point below
 * disregards its argument, but the game stores this pointer and tests it
 * against null to decide whether it has a surface at all, so it has to be a
 * real, stable, non-null value.
 */
extern "C" ANativeWindow *android_platform_window(void)
{
    return (ANativeWindow *)g_window;
}

extern "C" AInputQueue *android_platform_queue(void)
{
    if (!g_queue)
        g_queue = new AInputQueue();
    return g_queue;
}

/* ------------------------------------------------------------------ *
 * ANativeWindow — there is exactly one, and it is the SDL window.
 * ------------------------------------------------------------------ */
extern "C" int32_t ANativeWindow_getWidth(ANativeWindow *window)
{
    (void)window;
    return g_width;
}

extern "C" int32_t ANativeWindow_getHeight(ANativeWindow *window)
{
    (void)window;
    return g_height;
}

extern "C" int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window,
                                                    int32_t width, int32_t height,
                                                    int32_t format)
{
    /*
     * On Android this requests a different backbuffer size. Here the surface is
     * owned by SDL/EGL and fixed at the panel's 640x480, so this is accepted
     * and ignored: honouring it would fight the KMS mode.
     */
    (void)window; (void)width; (void)height; (void)format;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Event translation
 * ------------------------------------------------------------------ */
static AInputEvent *new_event(int32_t type, int32_t source)
{
    AInputEvent *ev = (AInputEvent *)calloc(1, sizeof(AInputEvent));
    ev->type     = type;
    ev->source   = source;
    ev->deviceId = 1;
    return ev;
}

static void push_key(int32_t keyCode, bool down)
{
    AInputEvent *ev = new_event(AINPUT_EVENT_TYPE_KEY, AINPUT_SOURCE_GAMEPAD);
    ev->action  = down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
    ev->keyCode = keyCode;
    android_platform_queue()->pending.push_back(ev);
}

/*
 * Two ways to speak to this game, selected by DEADSPACE_INPUT. Joystick is the
 * default, and the device settled which one is right.
 *
 * "touch" synthesises fingers: the port pretends to be a thumb on each half of
 * the screen. It is what this port did from the start, inherited from an earlier
 * 2, and on this build it cannot work at all. The APK is the OUYA release -
 * a console with no touchscreen - and the engine rejects touch outright:
 *
 *     [W/native-activity] WARNING: Unknown input source (4098)!
 *
 * 4098 is 0x1002, AINPUT_SOURCE_TOUCHSCREEN. Every synthetic finger was
 * discarded, which is why the on-screen cursor moved (this port draws it) but
 * nothing could ever be selected with it.
 *
 * "joystick" talks to the game as a controller, which is what this build was
 * made for. See AMotionEvent_getAxisValue() for how it reads the sticks - the
 * answer is not the obvious one, and getting it wrong killed the process.
 */
static bool input_is_joystick(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *env = getenv("DEADSPACE_INPUT");
        mode = (env && strcmp(env, "touch") == 0) ? 0 : 1;
        SDL_Log("input mode: %s", mode ? "joystick (native controller)" : "touch (synthetic fingers)");
    }
    return mode == 1;
}

/*
 * The current position of every joystick axis.
 *
 * On Android a joystick MotionEvent is a snapshot of the whole device: all
 * axes at once, not one event per axis. SDL reports them one at a time, so the
 * live values are kept here and every event carries the complete set. A game
 * that reads an axis this port never touched then gets a resting 0 rather than
 * whatever the last event happened to leave behind.
 */
static float g_axis[AMOTION_EVENT_MAX_AXIS];

static void push_joystick(float x, float y)
{
    g_axis[AMOTION_EVENT_AXIS_X] = x;
    g_axis[AMOTION_EVENT_AXIS_Y] = y;

    AInputEvent *ev = new_event(AINPUT_EVENT_TYPE_MOTION, AINPUT_SOURCE_JOYSTICK);
    ev->action       = AMOTION_EVENT_ACTION_MOVE;
    ev->pointerCount = 1;
    ev->pointerId[0] = 0;
    ev->x[0]         = x;
    ev->y[0]         = y;
    memcpy(ev->axis, g_axis, sizeof(ev->axis));
    android_platform_queue()->pending.push_back(ev);
}

static void push_motion(int32_t action, float x, float y, int32_t pointerId)
{
    AInputEvent *ev = new_event(AINPUT_EVENT_TYPE_MOTION, AINPUT_SOURCE_TOUCHSCREEN);
    ev->action       = action;
    ev->pointerCount = 1;
    ev->pointerId[0] = pointerId;
    ev->x[0]         = x;
    ev->y[0]         = y;
    android_platform_queue()->pending.push_back(ev);
}

/*
 * Which SDL button carries the letter the player sees. SDL names the face
 * buttons by position - its "A" is always the bottom one - while these
 * handhelds are silkscreened Nintendo style, with A on the right. Defined here
 * rather than next to face_layout_init() because map_button() needs them.
 * DEADSPACE_FACE_LAYOUT=xbox switches them for a handheld lettered the other way.
 */
static uint8_t g_fire_pad_button   = SDL_CONTROLLER_BUTTON_B;
static uint8_t g_cursor_pad_button = SDL_CONTROLLER_BUTTON_A;

static bool face_keys_are_legacy(void);

static int32_t map_button(int button)
{
    switch (button) {
    /*
     * The two face buttons, by silkscreen rather than by SDL's position.
     *
     * In touch mode these are the fire and cursor-tap buttons and never became
     * gamepad keys - B was left unmapped on purpose, because in an earlier port the
     * engine read it as something else entirely.
     *
     * In joystick mode that leaves the handheld's main button doing nothing,
     * which is exactly what the device showed: menus could only be accepted
     * with R1. So here the button a player reads as "A" sends BUTTON_A and the
     * one they read as "B" sends BUTTON_B, which is what a controller-era
     * build like this OUYA one expects. DEADSPACE_FACE_KEYS=legacy restores the
     * old behaviour if some menu turns out to disagree.
     */
    case SDL_CONTROLLER_BUTTON_A:
    case SDL_CONTROLLER_BUTTON_B:
        if (!input_is_joystick() || face_keys_are_legacy())
            return (button == SDL_CONTROLLER_BUTTON_A) ? AKEYCODE_BUTTON_A : 0;
        return (button == g_fire_pad_button) ? AKEYCODE_BUTTON_A : AKEYCODE_BUTTON_B;

    case SDL_CONTROLLER_BUTTON_X:             return AKEYCODE_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y:             return AKEYCODE_BUTTON_Y;
    /*
     * Start opens the game's menu, and the keycode for that is MENU, not BACK.
     *
     * BACK is the Android convention and it is what this port sent at first,
     * but pressing Start did nothing at all. The engine's key handler is a
     * jump table, not a chain of comparisons, which is why grepping the
     * disassembly for the constant finds nothing:
     *
     *     34444: sub  r4, r4, #19          ; keycode - 19
     *     34448: cmp  r4, #88              ; in range 19..107?
     *     3444c: addls pc, pc, r4, lsl #2  ; dispatch
     *     34450: b    <default>            ; everything else, silently
     *
     * BACK is 4, below the table, so it fell straight through to the default
     * case. MENU is 82, index 63, and has a real entry. BUTTON_START (108) is
     * one past the end of the table, so it would have been just as dead.
     */
    case SDL_CONTROLLER_BUTTON_START:         return AKEYCODE_MENU;
    case SDL_CONTROLLER_BUTTON_BACK:          return AKEYCODE_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return AKEYCODE_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AKEYCODE_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return AKEYCODE_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return AKEYCODE_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return AKEYCODE_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return AKEYCODE_DPAD_RIGHT;
    default:                                  return 0;
    }
}

/*
 * The Play build is a two-thumb touch game: a virtual stick under each thumb,
 * left to walk and right to aim and fire. Both are therefore driven the same
 * way - hold a finger down in that half of the screen and drag it - and the
 * only difference between them is which half and which pointer id.
 *
 * Pointer ids matter: the engine tracks the two thumbs independently, so they
 * must never share one.
 */
#define STICK_DEADZONE  8000
#define MOVE_POINTER_ID 0
#define AIM_POINTER_ID  1

struct VirtualStick {
    float centre_x_frac;
    float span_x_frac;
    int32_t pointer_id;
    bool held;
};

/*
 * The thumbs sit low on the screen, where a thumb actually rests - not at mid
 * height. Centring them vertically put the top of the aim stick's travel at
 * y=48 on a 480px panel, right on top of the HUD's pause button, so aiming
 * upward opened the menu by itself. Keeping the touches in the lower band
 * leaves the whole HUD untouched.
 */
#define STICK_CENTRE_Y 0.72f
#define STICK_SPAN_Y   0.16f

/*
 * Back to the layout the game documents: walk on the left, aim and fire on the
 * right. They had been swapped on a device report, but that reading turned out
 * to be a bug in the ROM rather than the game's arrangement - and the swap had
 * a second victim that made it obvious: the fire buttons put their finger down
 * in the aim thumb's half, so with the halves crossed A, R1 and R2 all walked
 * instead of shooting.
 *
 * The pointer ids stay with their roles; only the screen halves are stated
 * correctly now.
 */
static VirtualStick g_move = { 0.25f, 0.18f, MOVE_POINTER_ID, false };
static VirtualStick g_aim  = { 0.75f, 0.20f, AIM_POINTER_ID,  false };

static void cursor_dismiss(void);

/*
 * The fire thumb can be held down by a button as well as by the stick.
 *
 * The game ships with AUTO-AIM on, so the right thumb is not really aiming -
 * it is saying "shoot". A button expresses that far better than a stick, and
 * the two share one finger: the button decides whether the finger is down, the
 * stick decides where it points. Holding A alone fires at the neutral position
 * and lets auto-aim pick the target; nudging the stick aims manually; doing
 * both aims manually while firing.
 *
 * One finger, not two, because the engine tracks thumbs by pointer id and a
 * second finger in the same half would read as a pinch.
 */
/* Any of these held keeps the fire thumb down. They are tracked separately so
 * releasing one while another is held does not stop the shooting. */
static void fire_changed(void);

static bool g_fire_button  = false;  /* A  */
static bool g_trigger_fire = false;  /* R2 */
static bool g_shoulder_fire = false; /* R1 */
static int16_t g_aim_x = 0, g_aim_y = 0;

/*
 * Which physical button fires.
 *
 * SDL names the face buttons by position, Xbox style: BUTTON_A is the bottom
 * one, BUTTON_B the one on the right. What the plastic says is a different
 * matter and varies by handheld - the R36S and most Anbernic units are
 * silkscreened Nintendo style, with A on the right, so the button a player
 * calls "A" arrives here as SDL_CONTROLLER_BUTTON_B.
 *
 * Fire belongs on the button the player reads as A, so the mapping has to
 * follow the silkscreen rather than SDL. Nintendo lettering is the default
 * because that is what these devices ship with; DEADSPACE_FACE_LAYOUT=xbox
 * switches to the other one for a handheld whose A really is at the bottom.
 */
/* Escape hatch for the face-key mapping above, in case a menu disagrees. */
static bool face_keys_are_legacy(void)
{
    static int legacy = -1;
    if (legacy < 0) {
        const char *env = getenv("DEADSPACE_FACE_KEYS");
        legacy = (env && strcmp(env, "legacy") == 0) ? 1 : 0;
    }
    return legacy == 1;
}

static void face_layout_init(void)
{
    const char *layout = getenv("DEADSPACE_FACE_LAYOUT");
    if (layout && (!strcasecmp(layout, "xbox") || !strcasecmp(layout, "x"))) {
        g_fire_pad_button   = SDL_CONTROLLER_BUTTON_A;
        g_cursor_pad_button = SDL_CONTROLLER_BUTTON_B;
    }
    trace("face layout: fire on SDL button %d (%s lettering)",
          g_fire_pad_button,
          g_fire_pad_button == SDL_CONTROLLER_BUTTON_B ? "Nintendo" : "Xbox");
}

static void update_stick(VirtualStick *st, int16_t sx, int16_t sy)
{
    if (input_is_joystick()) {
        bool  dead = (abs(sx) <= STICK_DEADZONE && abs(sy) <= STICK_DEADZONE);
        float fx   = dead ? 0.0f : sx / 32767.0f;
        float fy   = dead ? 0.0f : sy / 32767.0f;

        if (st == &g_move) {
            push_joystick(fx, fy);
        } else {
            /* Android puts a second stick on Z/RZ, so that is where the aiming
             * one goes. Auto-aim means the game may never read it, which is
             * exactly why it costs nothing to send: the event already carries
             * every axis, and a game that does read it now finds it. */
            g_axis[AMOTION_EVENT_AXIS_Z]  = fx;
            g_axis[AMOTION_EVENT_AXIS_RZ] = fy;
            push_joystick(g_axis[AMOTION_EVENT_AXIS_X], g_axis[AMOTION_EVENT_AXIS_Y]);
        }

        if (!dead)
            cursor_dismiss();
        return;
    }

    bool active = (abs(sx) > STICK_DEADZONE || abs(sy) > STICK_DEADZONE);

    /* The fire button holds the aim thumb down even at rest. */
    if (st == &g_aim && (g_fire_button || g_trigger_fire || g_shoulder_fire))
        active = true;

    /* Sticks mean "playing", and the cursor has no business being on screen
     * then: it is a touch target the game can hit. Retire it immediately
     * rather than waiting for the idle timer. */
    if (active)
        cursor_dismiss();

    float cx = g_width  * st->centre_x_frac;
    float cy = g_height * STICK_CENTRE_Y;
    float px = cx + (sx / 32767.0f) * (g_width  * st->span_x_frac);
    float py = cy + (sy / 32767.0f) * (g_height * STICK_SPAN_Y);

    if (active && !st->held) {
        push_motion(AMOTION_EVENT_ACTION_DOWN, px, py, st->pointer_id);
        st->held = true;
    } else if (active) {
        push_motion(AMOTION_EVENT_ACTION_MOVE, px, py, st->pointer_id);
    } else if (st->held) {
        push_motion(AMOTION_EVENT_ACTION_UP, px, py, st->pointer_id);
        st->held = false;
    }
}

/* ------------------------------------------------------------------ *
 * Menu cursor
 *
 * The menus are plain touch targets: there is no gamepad navigation in this
 * build, so a pad alone cannot get past the title screen no matter how the
 * buttons are mapped. What is needed is a pointer, which is what PortMaster's
 * own guidance calls a software cursor for KMS/DRM.
 *
 * It rides the d-pad rather than a stick: the sticks are the two virtual
 * thumbs above and must stay free during play, while the d-pad is unused by
 * this game. A press of A taps wherever the cursor is.
 * ------------------------------------------------------------------ */
#define CURSOR_POINTER_ID 2
#define CURSOR_SPEED_PPS  420.0f     /* pixels per second at full tilt */

/*
 * The cursor exists for the menus and must get out of the way during play.
 * It was staying on screen forever once moved, and A tapped wherever it sat -
 * so a press while playing poked the HUD and opened the pause menu. It now
 * fades out after a few idle seconds, and A only taps while it is showing.
 */
#define CURSOR_IDLE_MS 2500

static float g_cursor_x = -1.0f, g_cursor_y = -1.0f;
static Uint32 g_cursor_used_ms = 0;
static bool  g_cursor_down = false;
static int   g_dpad_x = 0, g_dpad_y = 0;
static Uint32 g_cursor_last_ms = 0;

extern "C" void android_cursor_position(float *x, float *y, int *visible)
{
    /* DEADSPACE_CURSOR_TEST parks the cursor on screen with no pad attached,
     * so the headless harness can verify that drawing it leaves the game's GL
     * state intact. Restoring that state wrongly would corrupt every frame
     * after the first, which is not something to discover on the device. */
    static int forced = -1;
    if (forced < 0)
        forced = getenv("DEADSPACE_CURSOR_TEST") ? 1 : 0;
    if (forced && g_cursor_x < 0.0f) {
        g_cursor_x = g_width  * 0.5f;
        g_cursor_y = g_height * 0.5f;
    }

    if (x) *x = g_cursor_x;
    if (y) *y = g_cursor_y;
    if (visible) *visible = (g_cursor_x >= 0.0f) ? 1 : 0;
}

static void cursor_tick(void)
{
    Uint32 now = SDL_GetTicks();
    if (!g_cursor_last_ms)
        g_cursor_last_ms = now;
    float dt = (now - g_cursor_last_ms) / 1000.0f;
    g_cursor_last_ms = now;

    if (!g_dpad_x && !g_dpad_y) {
        /* Idle: retire the cursor so it stops being a target during play.
         * Never while it is held down, or a drag would be cut in half. */
        if (g_cursor_x >= 0.0f && !g_cursor_down &&
            g_cursor_used_ms && (now - g_cursor_used_ms) > CURSOR_IDLE_MS) {
            g_cursor_x = g_cursor_y = -1.0f;
        }
        return;
    }

    g_cursor_used_ms = now;

    if (g_cursor_x < 0.0f) {          /* first use: start at the centre */
        g_cursor_x = g_width  * 0.5f;
        g_cursor_y = g_height * 0.5f;
    }

    g_cursor_x += g_dpad_x * CURSOR_SPEED_PPS * dt;
    g_cursor_y += g_dpad_y * CURSOR_SPEED_PPS * dt;

    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_y < 0) g_cursor_y = 0;
    if (g_cursor_x > g_width  - 1) g_cursor_x = g_width  - 1;
    if (g_cursor_y > g_height - 1) g_cursor_y = g_height - 1;

    /* Dragging while held, so sliders and swipes work too. */
    if (g_cursor_down)
        push_motion(AMOTION_EVENT_ACTION_MOVE, g_cursor_x, g_cursor_y, CURSOR_POINTER_ID);
}

static void cursor_dismiss(void)
{
    if (g_cursor_down) {
        push_motion(AMOTION_EVENT_ACTION_UP, g_cursor_x, g_cursor_y, CURSOR_POINTER_ID);
        g_cursor_down = false;
    }
    g_cursor_x = g_cursor_y = -1.0f;
}

static void cursor_tap(bool down)
{
    g_cursor_used_ms = SDL_GetTicks();
    if (g_cursor_x < 0.0f) {
        g_cursor_x = g_width  * 0.5f;
        g_cursor_y = g_height * 0.5f;
    }
    push_motion(down ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP,
                g_cursor_x, g_cursor_y, CURSOR_POINTER_ID);
    g_cursor_down = down;
}

/* ------------------------------------------------------------------ *
 * Controllers
 *
 * Opening them is what makes SDL emit CONTROLLERBUTTON/AXIS events at all.
 * Without this, everything below is unreachable code — which is exactly the
 * state this port was in until now.
 * ------------------------------------------------------------------ */
#define MAX_PADS 4
static SDL_GameController *g_pads[MAX_PADS];

extern "C" void android_platform_open_controllers(void)
{
    int opened = 0;
    for (int i = 0; i < SDL_NumJoysticks() && opened < MAX_PADS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        SDL_GameController *c = SDL_GameControllerOpen(i);
        if (c) {
            g_pads[opened++] = c;
            SDL_Log("controller %d: %s", i, SDL_GameControllerName(c));
        }
    }
    SDL_Log("controllers opened: %d", opened);
}

static void pad_added(int index)
{
    for (int i = 0; i < MAX_PADS; i++) {
        if (!g_pads[i]) {
            g_pads[i] = SDL_GameControllerOpen(index);
            return;
        }
    }
}

static void pad_removed(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++) {
        if (!g_pads[i])
            continue;
        SDL_Joystick *j = SDL_GameControllerGetJoystick(g_pads[i]);
        if (j && SDL_JoystickInstanceID(j) == id) {
            SDL_GameControllerClose(g_pads[i]);
            g_pads[i] = NULL;
            return;
        }
    }
}

/* Defined with the looper below; the synthetic injector must not fire before it. */
static bool android_platform_input_attached(void);
extern "C" long android_egl_frames(void);

/*
 * Synthetic input, for verifying the engine responds to events without any
 * hardware attached. DEADSPACE_SYNTH_INPUT=<ms> taps the screen centre at that
 * interval. This exists because the harness runs headless: without it there is
 * no way to tell "the engine ignores our events" from "nothing sent any".
 * It is a test hook, never active unless the variable is set.
 */
static void synth_input_tick(void)
{
    static long  interval = -1;
    static Uint32 last = 0;
    static bool  down = false;

    if (interval < 0) {
        const char *env = getenv("DEADSPACE_SYNTH_INPUT");
        interval = env ? atol(env) : 0;
        if (interval > 0)
            SDL_Log("synthetic input every %ld ms", interval);
    }
    if (interval == 0)
        return;

    /*
     * Nothing may be queued before the app attaches its input queue. Pushing
     * early makes ALooper_pollAll report input the app is not listening for
     * yet; native_app_glue then reads its own empty command pipe and blocks.
     * Measured: injecting from frame 0 froze the game after a single frame,
     * against 2032 with no injection at all.
     */
    if (!android_platform_input_attached())
        return;

    /*
     * Attachment alone is not enough: the engine attaches its queue before it
     * finishes loading, and injecting during the load froze it at 482 of 811
     * assets. Wait for the game to be drawing steadily first.
     */
    if (android_egl_frames() < 600)
        return;

    Uint32 now = SDL_GetTicks();
    if (now - last < (Uint32)interval)
        return;
    last = now;

    float cx = g_width * 0.5f, cy = g_height * 0.5f;
    if (!down) {
        push_motion(AMOTION_EVENT_ACTION_DOWN, cx, cy, 0);
        push_key(AKEYCODE_BUTTON_A, true);
        down = true;
    } else {
        push_motion(AMOTION_EVENT_ACTION_UP, cx, cy, 0);
        push_key(AKEYCODE_BUTTON_A, false);
        down = false;
    }
}

/*
 * A fire button changed. In joystick mode the game gets a gamepad key and does
 * the rest itself; in touch mode the aim thumb has to be pressed or lifted.
 * Keeping this in one place is what stops the three fire buttons from
 * disagreeing about whether the finger is down.
 */
static void fire_changed(void)
{
    bool firing = (g_fire_button || g_trigger_fire || g_shoulder_fire);

    if (input_is_joystick()) {
        static bool sent = false;
        if (firing != sent) {
            push_key(AKEYCODE_BUTTON_A, firing);
            sent = firing;
        }
        if (firing)
            cursor_dismiss();
        return;
    }

    update_stick(&g_aim, g_aim_x, g_aim_y);
}

extern "C" bool android_platform_pump(void)
{
    SDL_Event e;
    bool keep_running = true;

    synth_input_tick();
    cursor_tick();

    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            keep_running = false;
            break;

        case SDL_CONTROLLERDEVICEADDED:
            pad_added(e.cdevice.which);
            break;

        case SDL_CONTROLLERDEVICEREMOVED:
            pad_removed(e.cdevice.which);
            break;

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            bool down = (e.type == SDL_CONTROLLERBUTTONDOWN);

            /* Which face button is which depends on the silkscreen, so these
             * two are resolved at startup and cannot be switch labels. */
            if (e.cbutton.button == g_fire_pad_button) {
                g_fire_button = down;
                fire_changed();
                continue;
            }
            if (e.cbutton.button == g_cursor_pad_button) {
                /* Confirms in the menus and does nothing during play, so a
                 * thumb resting on it cannot poke the screen. */
                if (g_cursor_x >= 0.0f || g_cursor_down)
                    cursor_tap(down);
                continue;
            }

            /* The d-pad drives the cursor rather than reaching the game: this
             * build has no d-pad navigation to reach. */
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  g_dpad_x = down ? -1 : 0; continue;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: g_dpad_x = down ?  1 : 0; continue;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:    g_dpad_y = down ? -1 : 0; continue;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  g_dpad_y = down ?  1 : 0; continue;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                g_shoulder_fire = down;
                fire_changed();
                continue;
            default: break;
            }

            int32_t code = map_button(e.cbutton.button);
            if (code)
                push_key(code, down);
            break;
        }

        case SDL_CONTROLLERAXISMOTION:
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX ||
                e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY) {
                static int16_t rx = 0, ry = 0;
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX) rx = e.caxis.value;
                else                                            ry = e.caxis.value;
                update_stick(&g_aim, rx, ry);
                g_aim_x = rx; g_aim_y = ry;
            } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ||
                       e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                /* Walking. This was missing entirely: only the aiming thumb
                 * had ever been mapped, so the character could not move. */
                static int16_t lx = 0, ly = 0;
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) lx = e.caxis.value;
                else                                           ly = e.caxis.value;
                update_stick(&g_move, lx, ly);
            } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
                static bool held = false;
                bool now = e.caxis.value > 16000;
                if (now != held) { push_key(AKEYCODE_BUTTON_L2, now); held = now; }
            } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                /* R2 fires too. It is an analog axis, so it needs its own
                 * threshold rather than a button event, and it shares the fire
                 * state with A: whichever is held keeps the thumb down, and
                 * releasing one while the other is held must not stop firing. */
                static bool held = false;
                bool now = e.caxis.value > 12000;
                if (now != held) {
                    held = now;
                    g_trigger_fire = now;
                    fire_changed();
                }
            }
            break;

        /* Touch is kept working for devices that have it. */
        case SDL_FINGERDOWN:
            push_motion(AMOTION_EVENT_ACTION_DOWN,
                        e.tfinger.x * g_width, e.tfinger.y * g_height, 0);
            break;
        case SDL_FINGERMOTION:
            push_motion(AMOTION_EVENT_ACTION_MOVE,
                        e.tfinger.x * g_width, e.tfinger.y * g_height, 0);
            break;
        case SDL_FINGERUP:
            push_motion(AMOTION_EVENT_ACTION_UP,
                        e.tfinger.x * g_width, e.tfinger.y * g_height, 0);
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            /* MENU rather than BACK for the same reason as Start: this engine's
             * key table starts at 19, so BACK never reaches anything. */
            if (e.key.keysym.sym == SDLK_ESCAPE)
                push_key(AKEYCODE_MENU, e.type == SDL_KEYDOWN);
            break;

        default:
            break;
        }
    }

    return keep_running;
}

/* ------------------------------------------------------------------ *
 * ALooper
 *
 * This has to be a real poll() loop, not a stub. The game is built on
 * native_app_glue, which creates a pipe and registers the read end via
 * ALooper_addFd to receive lifecycle commands. A looper that ignored fds
 * would leave the game blocked forever waiting for APP_CMD_INIT_WINDOW.
 * ------------------------------------------------------------------ */
#include <poll.h>

#define MAX_LOOPER_FDS  8

/*
 * The identifier ALooper_pollAll returns for input.
 *
 * It is not ours to choose: the app picks it in AInputQueue_attachLooper, and
 * native_app_glue uses 1 for its own command pipe. Returning a guessed 1 would
 * make the game read its command pipe with nothing in it and block forever, so
 * nothing is reported as input until the app has actually attached its queue
 * and told us which number it wants back.
 */
static int  g_input_ident    = -1;
static void *g_input_data    = NULL;
static bool g_input_attached = false;

static bool android_platform_input_attached(void) { return g_input_attached; }

struct LooperFd {
    int                  fd;
    int                  ident;
    int                  events;
    ALooper_callbackFunc callback;
    void                *data;
};

static ALooper  *g_looper = (ALooper *)0x10000001;  /* opaque, never dereferenced */
static LooperFd  g_fds[MAX_LOOPER_FDS];
static int       g_num_fds = 0;

extern "C" ALooper *ALooper_prepare(int opts)
{
    (void)opts;
    return g_looper;
}

extern "C" int ALooper_addFd(ALooper *looper, int fd, int ident, int events,
                             ALooper_callbackFunc callback, void *data)
{
    (void)looper;
    if (g_num_fds >= MAX_LOOPER_FDS)
        return -1;

    /* Re-registering an fd replaces the old entry, as the NDK does. */
    for (int i = 0; i < g_num_fds; i++) {
        if (g_fds[i].fd == fd) {
            g_fds[i] = { fd, ident, events, callback, data };
            return 1;
        }
    }

    g_fds[g_num_fds++] = { fd, ident, events, callback, data };
    return 1;
}

extern "C" void android_opensles_pump(void);

extern "C" int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
    /*
     * Liveness probe for the freeze at ~2032 frames: if the game stops
     * presenting but keeps calling in here, it is alive in its event loop and
     * the fault is downstream of input. If these stop too, it is blocked
     * somewhere else entirely.
     */
    {
        static long calls = 0;
        static bool probe = getenv("DEADSPACE_POLL_PROBE") != NULL;
        if (probe && (++calls % 500) == 0)
            trace("poll alive: %ld calls, %ld frames", calls, android_egl_frames());
    }

    if (!android_platform_pump())
        return ALOOPER_POLL_ERROR;      /* window closed: let the game unwind */

    /* The audio buffer queues are retired here, on the game's own thread, for
     * the reason spelled out above android_opensles_pump(). */
    android_opensles_pump();

    /* Input we synthesised takes priority and needs no fd. */
    if (g_input_attached && !android_platform_queue()->pending.empty()) {
        if (outFd)     *outFd = -1;
        if (outEvents) *outEvents = ALOOPER_EVENT_INPUT;
        if (outData)   *outData = g_input_data;
        return g_input_ident;
    }

    if (g_num_fds > 0) {
        struct pollfd pfds[MAX_LOOPER_FDS];
        for (int i = 0; i < g_num_fds; i++) {
            pfds[i].fd = g_fds[i].fd;
            pfds[i].events = 0;
            if (g_fds[i].events & ALOOPER_EVENT_INPUT)  pfds[i].events |= POLLIN;
            if (g_fds[i].events & ALOOPER_EVENT_OUTPUT) pfds[i].events |= POLLOUT;
            pfds[i].revents = 0;
        }

        /*
         * Cap the wait at 1 ms even when the game asks to block forever: it
         * renders from this same loop, and SDL events must keep being pumped.
         */
        int wait = (timeoutMillis < 0 || timeoutMillis > 1) ? 1 : timeoutMillis;

        if (poll(pfds, g_num_fds, wait) > 0) {
            for (int i = 0; i < g_num_fds; i++) {
                if (!pfds[i].revents)
                    continue;

                int ev = 0;
                if (pfds[i].revents & POLLIN)  ev |= ALOOPER_EVENT_INPUT;
                if (pfds[i].revents & POLLOUT) ev |= ALOOPER_EVENT_OUTPUT;
                if (pfds[i].revents & POLLERR) ev |= ALOOPER_EVENT_ERROR;
                if (pfds[i].revents & POLLHUP) ev |= ALOOPER_EVENT_HANGUP;

                if (g_fds[i].callback) {
                    /* Callback-style registration: run it and keep polling. */
                    if (g_fds[i].callback(g_fds[i].fd, ev, g_fds[i].data) == 0) {
                        g_fds[i] = g_fds[--g_num_fds];
                    }
                    return ALOOPER_POLL_CALLBACK;
                }

                if (outFd)     *outFd = g_fds[i].fd;
                if (outEvents) *outEvents = ev;
                if (outData)   *outData = g_fds[i].data;
                return g_fds[i].ident;
            }
        }
    } else if (timeoutMillis != 0) {
        SDL_Delay(1);
    }

    return ALOOPER_POLL_TIMEOUT;
}

/* ------------------------------------------------------------------ *
 * AInputQueue
 * ------------------------------------------------------------------ */
extern "C" int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent)
{
    AInputQueue *q = queue ? queue : android_platform_queue();
    if (q->pending.empty())
        return -1;

    *outEvent = q->pending.front();
    q->pending.pop_front();
    return 0;
}

extern "C" int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event)
{
    /* 0 == "not pre-dispatched, deliver it to the app". */
    (void)queue; (void)event;
    return 0;
}

extern "C" void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled)
{
    (void)queue; (void)handled;
    free(event);
}

extern "C" void AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident,
                                         ALooper_callbackFunc callback, void *data)
{
    /*
     * There is no fd behind this queue - the events are synthesised from SDL -
     * but two things the app passes here are load-bearing and both must come
     * back out of ALooper_pollAll: the identifier, and the data pointer.
     *
     * The data pointer is the app's own poll source. native_app_glue runs
     *
     *     while ((ident = ALooper_pollAll(..., (void**)&source)) >= 0)
     *         if (source != NULL) source->process(app, source);
     *
     * so handing back NULL means the event is never processed, the queue is
     * never drained, pollAll keeps returning a valid ident, and the game spins
     * in that loop without ever reaching its draw. Measured: the game froze on
     * the first synthetic event regardless of when it was sent - after 1 frame
     * when injecting from the start, after 600 when injecting from frame 600.
     */
    (void)queue; (void)looper; (void)callback;
    g_input_ident    = ident;
    g_input_data     = data;
    g_input_attached = true;
}

extern "C" void AInputQueue_detachLooper(AInputQueue *queue)
{
    (void)queue;
    g_input_attached = false;
}

/* ------------------------------------------------------------------ *
 * Event accessors
 * ------------------------------------------------------------------ */
extern "C" int32_t AInputEvent_getType(const AInputEvent *e)     { return e ? e->type : 0; }
extern "C" int32_t AInputEvent_getSource(const AInputEvent *e)   { return e ? e->source : 0; }
extern "C" int32_t AInputEvent_getDeviceId(const AInputEvent *e) { return e ? e->deviceId : 0; }

extern "C" int32_t AKeyEvent_getAction(const AInputEvent *e)     { return e ? e->action : 0; }
extern "C" int32_t AKeyEvent_getFlags(const AInputEvent *e)      { return e ? e->flags : 0; }
extern "C" int32_t AKeyEvent_getKeyCode(const AInputEvent *e)    { return e ? e->keyCode : 0; }
extern "C" int32_t AKeyEvent_getMetaState(const AInputEvent *e)  { return e ? e->metaState : 0; }

extern "C" int32_t AMotionEvent_getAction(const AInputEvent *e)    { return e ? e->action : 0; }
extern "C" int32_t AMotionEvent_getFlags(const AInputEvent *e)     { return e ? e->flags : 0; }
extern "C" int32_t AMotionEvent_getMetaState(const AInputEvent *e) { return e ? e->metaState : 0; }

extern "C" size_t AMotionEvent_getPointerCount(const AInputEvent *e)
{
    return e ? e->pointerCount : 0;
}

extern "C" int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t idx)
{
    return (e && idx < e->pointerCount) ? e->pointerId[idx] : 0;
}

extern "C" float AMotionEvent_getX(const AInputEvent *e, size_t idx)
{
    return (e && idx < e->pointerCount) ? e->x[idx] : 0.0f;
}

extern "C" float AMotionEvent_getY(const AInputEvent *e, size_t idx)
{
    return (e && idx < e->pointerCount) ? e->y[idx] : 0.0f;
}

/*
 * The accessor whose absence killed the process.
 *
 * It is not in the game's PLT, so reading the import table says the game never
 * calls it - the conclusion this port shipped with, and it was wrong. The name
 * is in the binary as a string, and so is dlsym: getAxisValue arrived in API 9
 * and getHistoricalAxisValue in API 13, later than the minimum this 2013 build
 * targets, so the engine looks them up at runtime and caches the pointers in a
 * table. That is the ordinary NDK pattern for an API that might be missing.
 *
 * Our dlsym answers out of the same symbol tables the loader links against, so
 * a name absent from them silently returns NULL - and the game does not check.
 * The first time a stick moved:
 *
 *     34324: cmp  r8, r3        ; is this event AINPUT_SOURCE_JOYSTICK?
 *     34330: ldr  r3, [r6, #4]  ; the cached getAxisValue pointer
 *     3433c: blx  r3            ; NULL. pc = 0, SIGSEGV.
 *
 * Registering these two in symtable_android is the whole fix: dlsym finds
 * them, the table holds real pointers, and the call lands somewhere.
 *
 * X and Y defer to the pointer-indexed arrays so that a touch event - where
 * each finger has its own coordinates - keeps answering per pointer. Every
 * other axis belongs to the device as a whole, so the pointer index only has
 * to be in range.
 */
extern "C" float AMotionEvent_getAxisValue(const AInputEvent *e, int32_t axis, size_t idx)
{
    /* DEADSPACE_INPUTTRACE=1 records which axes the game actually asks for. An
     * axis this port does not fill answers a resting 0, which is safe but is
     * also indistinguishable from "the stick is centred" - so when input
     * behaves oddly, this says whether the question was even understood. */
    if (getenv("DEADSPACE_INPUTTRACE"))
        trace("getAxisValue(axis=%d, idx=%u) source=%d",
              axis, (unsigned)idx, e ? e->source : -1);

    if (!e)
        return 0.0f;
    if (axis == AMOTION_EVENT_AXIS_X)
        return AMotionEvent_getX(e, idx);
    if (axis == AMOTION_EVENT_AXIS_Y)
        return AMotionEvent_getY(e, idx);
    if (axis < 0 || axis >= AMOTION_EVENT_MAX_AXIS || idx >= e->pointerCount)
        return 0.0f;
    return e->axis[axis];
}

/* No historical samples are batched, so the history is always empty and the
 * "historical" accessors just return the current position. */
extern "C" size_t AMotionEvent_getHistorySize(const AInputEvent *e)
{
    (void)e;
    return 0;
}

extern "C" float AMotionEvent_getHistoricalX(const AInputEvent *e, size_t idx, size_t pos)
{
    (void)pos;
    return AMotionEvent_getX(e, idx);
}

extern "C" float AMotionEvent_getHistoricalY(const AInputEvent *e, size_t idx, size_t pos)
{
    (void)pos;
    return AMotionEvent_getY(e, idx);
}

extern "C" float AMotionEvent_getHistoricalAxisValue(const AInputEvent *e, int32_t axis,
                                                     size_t idx, size_t pos)
{
    (void)pos;
    return AMotionEvent_getAxisValue(e, axis, idx);
}

/* ------------------------------------------------------------------ *
 * native_activity.h — the one call the game makes on itself.
 * ------------------------------------------------------------------ */
/* The game calls this to end the activity (quit from its own menu, or a fatal
 * error inside the engine). Android tears the process down afterwards; here the
 * request is recorded and the frame loop consumes it, so shutdown stays on our
 * thread - see app_exit.h. Routing it there rather than to a flag of our own is
 * what gives this path the SDL_QUIT wakeup and the trace line: the flag it used
 * to raise had no reader at all, so a game that left through NativeActivity
 * instead of MainActivity.finish() froze exactly as if nothing had happened. */
extern "C" void ANativeActivity_finish(ANativeActivity *activity)
{
    (void)activity;
    android_app_request_exit("ANativeActivity_finish()");
}

/* The same question under the platform layer's own name. */
extern "C" bool android_platform_finished(void)
{
    return android_app_exit_requested();
}
