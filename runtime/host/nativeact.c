// nativeact.c: the NativeActivity host of ua_host (UA_NATIVEACTIVITY=1). A game whose engine is
// its own Android NativeActivity library (Broken Sword Reforged's libBS1R.so) is started the way
// Android's framework starts it: a filled ANativeActivity, ANativeActivity_onCreate(), then the
// lifecycle callbacks while the engine runs its own thread on ALooper/EGL/OpenSL ES.
//   - the EGL window is ua_host's (display.c / egl_glue.c, ANativeWindow from native_window.c)
//   - assets: the APK's stored `assets/` members (asset_manager.c's android_set_asset_apk)
//   - input: SDL keyboard/pad/mouse/touch -> AInputEvent, queued in a real AInputQueue that wakes
//     the engine's looper (looper_input_sensor.c)
//   - Java: fakejni, the engine's activity class in up_nativeact.c
// The lifecycle is Android's: onStart, onResume, onNativeWindowCreated, onInputQueueCreated,
// onWindowFocusChanged(1); at exit onWindowFocusChanged(0), onPause, onStop,
// onNativeWindowDestroyed, onDestroy.
#define _GNU_SOURCE
#include "nativeact.h"

#include "../loader/so_loader.h"
#include "../android/android_shims.h"
#include "../android/egl_glue.h"
#include "../jni/fakejni.h"
#include "display.h"
#include "pointer.h"

#include <SDL2/SDL.h>

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- the NDK's android/native_activity.h (no NDK headers here) ------------------------------- */
typedef struct ANativeActivity ANativeActivity;

typedef struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity *activity);
    void (*onResume)(ANativeActivity *activity);
    void *(*onSaveInstanceState)(ANativeActivity *activity, size_t *outLen);
    void (*onPause)(ANativeActivity *activity);
    void (*onStop)(ANativeActivity *activity);
    void (*onDestroy)(ANativeActivity *activity);
    void (*onWindowFocusChanged)(ANativeActivity *activity, int hasFocus);
    void (*onNativeWindowCreated)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowResized)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowDestroyed)(ANativeActivity *activity, ANativeWindow *window);
    void (*onInputQueueCreated)(ANativeActivity *activity, AInputQueue *queue);
    void (*onInputQueueDestroyed)(ANativeActivity *activity, AInputQueue *queue);
    void (*onContentRectChanged)(ANativeActivity *activity, const ARect *rect);
    void (*onConfigurationChanged)(ANativeActivity *activity);
    void (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    void *vm;                       /* JavaVM* */
    void *env;                      /* JNIEnv* */
    void *clazz;                    /* jobject */
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t sdkVersion;
    void *instance;                 /* the app's own state (it sets this) */
    void *assetManager;             /* AAssetManager* */
    const char *obbPath;
};

typedef void (*nativeactivity_oncreate_fn)(ANativeActivity *activity, void *savedState, size_t savedStateSize);

extern jobject up_nativeact_activity(void);     /* runtime/jni/up_nativeact.c */
extern int     up_nativeact_obb_pending(void);  /* runtime/jni/up_nativeact.c */

/* ---- state ----------------------------------------------------------------------------------- */
static volatile sig_atomic_t g_finish;
static AInputQueue *g_queue;
static int g_window_w = 640, g_window_h = 480;

/* NA_POINTER=1: the right stick moves a virtual cursor over the window and A/R1/RT click at it with
 * touch events (see na_pointer_tick and on_sdl_event). Off by default: nothing changes then. The
 * position the render thread reads is published as ints; g_ptr_px/py are the loop thread's exact. */
static int g_ptr_on, g_ptr_cursor_on;
static float g_ptr_px, g_ptr_py;                /* window pixels, loop thread */
static volatile int g_ptr_x, g_ptr_y;           /* the same, rounded, for na_swap_hook */
static float g_ptr_rx, g_ptr_ry;                /* right stick, -1..1 */
static unsigned g_ptr_clicks;                   /* click controls held (PTR_A/PTR_R1/PTR_RT) */
static double g_ptr_last, g_ptr_sent, g_ptr_full;

/* egl_glue.c's swap hook (eglSwapBuffers of the engine's thread): proof that frames are presented,
 * and where the cursor is drawn (the engine's GL context is current here, just before the swap) */
static void na_swap_hook(void *ctx)
{
    static unsigned n;
    (void)ctx;
    n++;
    if (n == 1) fprintf(stderr, "[na] first swap\n");
    if (n % 60 == 0) fprintf(stderr, "[na] swap %u\n", n);
    if (g_ptr_on && g_ptr_cursor_on) pointer_draw_at(g_ptr_x, g_ptr_y, g_window_w, g_window_h);
}

static void on_activity_finish(void *activity) { (void)activity; g_finish = 1; }
static void on_signal(int s) { (void)s; g_finish = 1; }

/* The engine's OBBFetch is dex code that returns immediately and then waits for this exported
 * callback of libBS1R; a phone without an OBB would never see it. The assets are in the APK, so
 * report completion once, on this thread (the activity thread), from the lifecycle or the looper:
 * never from inside the engine's own Java call. */
static void (*g_obb_complete)(void *env, void *activity);
static int g_obb_fired;

static void na_fire_obb_complete(void *env, void *activity)
{
    if (g_obb_fired || !g_obb_complete || !up_nativeact_obb_pending()) return;
    g_obb_fired = 1;
    fprintf(stderr, "[na] OBBFetchComplete\n");
    g_obb_complete(env, activity);
}

static void mkdir_p(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(buf, 0755); *p = '/'; }
    mkdir(buf, 0755);
}

/* the APK: NA_APK, else android_package.json's "apk" (passed in), else the only .apk in root */
static int find_apk(const char *root, const char *apk_name, char *out, size_t n)
{
    const char *e = getenv("NA_APK");
    if (e && *e) { snprintf(out, n, "%s", e); return 1; }
    if (apk_name && *apk_name) { snprintf(out, n, "%s/%s", root, apk_name); return access(out, R_OK) == 0; }
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l > 4 && !strcmp(de->d_name + l - 4, ".apk")) {
            if (found) { found = 0; break; }     /* more than one: no guess */
            snprintf(out, n, "%s/%s", root, de->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* ---- SDL -> Android input ------------------------------------------------------------------- */
/* Android key codes for what a handheld sends: its pad's buttons, a keyboard, the volume keys */
static int32_t key_of(SDL_Keycode k)
{
    switch (k) {
    case SDLK_BACKSPACE: return 67;     /* AKEYCODE_DEL */
    case SDLK_RETURN: case SDLK_KP_ENTER: return 66;
    case SDLK_ESCAPE: return 4;         /* AKEYCODE_BACK */
    case SDLK_SPACE: return 62;
    case SDLK_UP: return 19;
    case SDLK_DOWN: return 20;
    case SDLK_LEFT: return 21;
    case SDLK_RIGHT: return 22;
    case SDLK_TAB: return 61;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 59;   /* SHIFT_LEFT */
    case SDLK_LCTRL: case SDLK_RCTRL: return 113;    /* CTRL_LEFT */
    case SDLK_LALT: case SDLK_RALT: return 57;       /* ALT_LEFT */
    default: return 0;
    }
}

static int32_t padkey_of(Uint8 b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return 96;
    case SDL_CONTROLLER_BUTTON_B: return 97;
    case SDL_CONTROLLER_BUTTON_X: return 99;
    case SDL_CONTROLLER_BUTTON_Y: return 100;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 102;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 103;
    case SDL_CONTROLLER_BUTTON_BACK: return 109;     /* SELECT */
    case SDL_CONTROLLER_BUTTON_START: return 108;
    case SDL_CONTROLLER_BUTTON_GUIDE: return 110;    /* MODE */
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 106;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 107;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 19;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 20;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 21;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 22;
    default: return 0;
    }
}

static void push_key(int32_t action, int32_t keycode)
{
    if (!g_queue || !keycode) return;
    android_input_queue_push(g_queue, android_input_event_new_key(0, action, keycode, 0));
}

static void push_motion(int32_t action, int x, int y)
{
    if (!g_queue) return;
    android_input_queue_push(g_queue, android_input_event_new_motion(0, action, x, y));
}

/* ---- NA_POINTER: the right stick's virtual cursor ------------------------------------------- */
/* The engine in TOUCH mode turns a touch down/up pair into a tap and draws no cursor of its own, so
 * the host draws one and clicks for it. Touch coordinates are window pixels, the same space the
 * mouse events above use. Clicks are the click controls only; no motion events are sent while the
 * cursor idles (the engine would read a hover as a finger). */
enum { PTR_A = 1, PTR_R1 = 2, PTR_RT = 4 };
#define PTR_DEADZONE (8000.0f / 32768.0f)   /* per axis */
#define PTR_SPEED    500.0f                 /* px/s at full deflection (a 640x480 window) */
#define PTR_FULL_S   0.4                    /* full deflection this long doubles the speed */

static void na_pointer_click(unsigned bit, int down)
{
    if (down) {
        if (!g_ptr_clicks) push_motion(0, g_ptr_x, g_ptr_y);       /* AKEY_EVENT_ACTION_DOWN */
        g_ptr_clicks |= bit;
    } else if (g_ptr_clicks & bit) {
        g_ptr_clicks &= ~bit;
        if (!g_ptr_clicks) push_motion(1, g_ptr_x, g_ptr_y);       /* AKEY_EVENT_ACTION_UP */
    }
}

/* the loop's tick (the ~4 ms poll): moves the cursor by the real elapsed time, so the speed does
 * not follow the loop's or the engine's frame rate */
static void na_pointer_tick(void)
{
    if (!g_ptr_on) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    float dt = g_ptr_last > 0 ? (float)(now - g_ptr_last) : 0.004f;
    g_ptr_last = now;
    if (dt > 0.1f) dt = 0.1f;                   /* a stall must not throw the cursor across */

    float rx = g_ptr_rx, ry = g_ptr_ry;
    if (fabsf(rx) < PTR_DEADZONE) rx = 0;
    if (fabsf(ry) < PTR_DEADZONE) ry = 0;
    if (rx || ry) {
        float m = sqrtf(rx * rx + ry * ry);
        if (m > 1) { rx /= m; ry /= m; m = 1; }
        g_ptr_full = m >= 0.99f ? g_ptr_full + dt : 0;
        float v = PTR_SPEED * m * (g_ptr_full >= PTR_FULL_S ? 2.0f : 1.0f);
        g_ptr_px += rx * v * dt;
        g_ptr_py += ry * v * dt;
        if (g_ptr_px < 0) g_ptr_px = 0; else if (g_ptr_px > g_window_w - 1) g_ptr_px = g_window_w - 1;
        if (g_ptr_py < 0) g_ptr_py = 0; else if (g_ptr_py > g_window_h - 1) g_ptr_py = g_window_h - 1;
        int nx = (int)lroundf(g_ptr_px), ny = (int)lroundf(g_ptr_py);
        if (g_ptr_clicks && (nx != g_ptr_x || ny != g_ptr_y) && now - g_ptr_sent >= 0.016) {
            g_ptr_sent = now;                   /* AKEY_EVENT_ACTION_MOVE while held */
            push_motion(2, nx, ny);
        }
    }
    g_ptr_x = (int)lroundf(g_ptr_px);
    g_ptr_y = (int)lroundf(g_ptr_py);
}

static int na_pointer_button(Uint8 b)
{
    return b == SDL_CONTROLLER_BUTTON_A ? PTR_A : b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ? PTR_R1 : 0;
}

static void on_sdl_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT:
        g_finish = 1;
        break;
    case SDL_KEYDOWN:
        if (!e->key.repeat) push_key(0, key_of(e->key.keysym.sym));   /* AKEY_EVENT_ACTION_DOWN */
        break;
    case SDL_KEYUP:
        push_key(1, key_of(e->key.keysym.sym));
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        if (g_ptr_on && na_pointer_button(e->cbutton.button)) {
            na_pointer_click(na_pointer_button(e->cbutton.button), 1);
            break;
        }
        push_key(0, padkey_of(e->cbutton.button));
        break;
    case SDL_CONTROLLERBUTTONUP:
        if (g_ptr_on && na_pointer_button(e->cbutton.button)) {
            na_pointer_click(na_pointer_button(e->cbutton.button), 0);
            break;
        }
        push_key(1, padkey_of(e->cbutton.button));
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (!g_ptr_on) break;
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX) g_ptr_rx = e->caxis.value / 32768.0f;
        else if (e->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY) g_ptr_ry = e->caxis.value / 32768.0f;
        else if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            na_pointer_click(PTR_RT, e->caxis.value > 16000);
        break;
    case SDL_MOUSEMOTION:
        if (e->motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK | SDL_BUTTON_MMASK))
            push_motion(2, e->motion.x, e->motion.y);                 /* AKEY_EVENT_ACTION_MOVE */
        break;
    case SDL_MOUSEBUTTONDOWN:
        push_motion(0, e->button.x, e->button.y);
        break;
    case SDL_MOUSEBUTTONUP:
        push_motion(1, e->button.x, e->button.y);
        break;
    case SDL_FINGERDOWN:
        push_motion(0, (int)(e->tfinger.x * g_window_w), (int)(e->tfinger.y * g_window_h));
        break;
    case SDL_FINGERUP:
        push_motion(1, (int)(e->tfinger.x * g_window_w), (int)(e->tfinger.y * g_window_h));
        break;
    case SDL_FINGERMOTION:
        push_motion(2, (int)(e->tfinger.x * g_window_w), (int)(e->tfinger.y * g_window_h));
        break;
    default:
        break;
    }
}

static void open_pad(int device_index)
{
    if (!SDL_IsGameController(device_index)) return;
    SDL_GameController *gc = SDL_GameControllerOpen(device_index);
    if (gc) fprintf(stderr, "[nativeact] pad: %s\n", SDL_GameControllerName(gc));
}

/* ---- the run --------------------------------------------------------------------------------- */
int nativeact_run(const char *root, const char *apk_name)
{
    // the engine: NA_LIB, else <root>/lib/libBS1R.so
    char lib[1024];
    const char *e = getenv("NA_LIB");
    if (e && *e) snprintf(lib, sizeof lib, "%s", e);
    else {
        const char *ld = getenv("UA_LIBDIR");
        snprintf(lib, sizeof lib, "%s/lib/libBS1R.so", ld && *ld ? ld : root);
    }

    char apk[1024] = "";
    if (!find_apk(root, apk_name, apk, sizeof apk)) apk[0] = 0;
    const char *asset_dir = getenv("NA_ASSET_DIR");
    if (apk[0]) {
        if (android_set_asset_apk(apk) == 0)
            fprintf(stderr, "[nativeact] assets from %s\n", apk);
        else if (asset_dir && *asset_dir)
            android_set_asset_dir(asset_dir);
        else
            android_set_asset_dir("assets");
    } else if (asset_dir && *asset_dir) {
        android_set_asset_dir(asset_dir);
    } else {
        android_set_asset_dir("assets");
    }

    // directories the engine writes saves/cache to (ANativeActivity's data paths)
    char data_dir[1024], files_dir[1024], ext_dir[1024];
    snprintf(data_dir, sizeof data_dir, "%s/data", root);
    snprintf(files_dir, sizeof files_dir, "%s/files", data_dir);
    snprintf(ext_dir, sizeof ext_dir, "%s/external", data_dir);
    mkdir_p(files_dir);
    mkdir_p(ext_dir);
    mkdir_p(data_dir);

    // the display (fbdev or an SDL window) was opened by ua_host main before this host started
    ANativeWindow *win = android_window_get();
    if (win) {
        g_window_w = ANativeWindow_getWidth(win);
        g_window_h = ANativeWindow_getHeight(win);
        if (g_window_w <= 0) g_window_w = 640;
        if (g_window_h <= 0) g_window_h = 480;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    android_set_activity_finish(on_activity_finish);
    fj_init();

    // the API level the app is told, and which libandroid functions exist (before the loader
    // indexes the shim tables)
    int sdk_int = getenv("NA_SDK_INT") ? atoi(getenv("NA_SDK_INT")) : 29;
    if (sdk_int < 9) sdk_int = 9;
    android_shims_set_api_level(sdk_int);

    // NA_POINTER=1: the right stick moves a cursor; A/R1/RT click it with touch events. NA_CURSOR=0
    // keeps the cursor undrawn (a game that shows its own). Both off/unread: the input is as before.
    const char *np = getenv("NA_POINTER");
    g_ptr_on = np && *np && strcmp(np, "0") != 0;
    const char *nc = getenv("NA_CURSOR");
    g_ptr_cursor_on = !(nc && !strcmp(nc, "0"));
    g_ptr_px = (g_window_w - 1) * 0.5f;
    g_ptr_py = (g_window_h - 1) * 0.5f;
    g_ptr_x = (int)lroundf(g_ptr_px);
    g_ptr_y = (int)lroundf(g_ptr_py);
    if (g_ptr_on)
        fprintf(stderr, "[nativeact] NA_POINTER: right stick moves a cursor, A/R1/RT click at it\n");

    // SDL gamepads (the window itself is display_sdl.c's)
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        SDL_GameControllerEventState(SDL_ENABLE);
        for (int i = 0, n = SDL_NumJoysticks(); i < n; i++) open_pad(i);
    }

    so_module *m = so_load(lib, NULL);
    if (!m) { fprintf(stderr, "[nativeact] %s: %s\n", lib, so_error()); return 1; }
    so_run_init(m);
    nativeactivity_oncreate_fn create = (nativeactivity_oncreate_fn)so_sym(m, "ANativeActivity_onCreate");
    if (!create) { fprintf(stderr, "[nativeact] %s: no ANativeActivity_onCreate\n", lib); return 1; }
    jint (*onload)(void *vm, void *reserved) = (jint (*)(void *, void *))so_sym(m, "JNI_OnLoad");
    if (onload) {
        jint v = onload(fj_vm(), NULL);
        fprintf(stderr, "[nativeact] JNI_OnLoad -> %d\n", (int)v);
    }
    g_obb_complete = (void (*)(void *, void *))so_sym(m, "Java_uk_co_cyborg_napp_MainActivity_OBBFetchComplete");
    if (!g_obb_complete) fprintf(stderr, "[nativeact] no OBBFetchComplete export in %s\n", lib);

    static ANativeActivityCallbacks cbs;
    static ANativeActivity act;
    static char iap[1024], eap[1024], obb[1024];
    obb[0] = 0;
    memset(&cbs, 0, sizeof cbs);
    memset(&act, 0, sizeof act);
    act.callbacks = &cbs;
    act.vm = fj_vm();
    act.env = fj_env();
    act.clazz = up_nativeact_activity();
    act.internalDataPath = iap;
    act.externalDataPath = eap;
    act.sdkVersion = sdk_int;
    act.assetManager = AAssetManager_fromJava(act.env, NULL);
    const char *ob = getenv("NA_OBB");
    if (ob && *ob) {
        snprintf(obb, sizeof obb, "%s", ob);
        act.obbPath = obb;
    }
    snprintf(iap, sizeof iap, "%s", files_dir);
    snprintf(eap, sizeof eap, "%s", ext_dir);

    fprintf(stderr, "[nativeact] %s, window %dx%d, API %d, assets from %s\n", lib, g_window_w, g_window_h,
            (int)act.sdkVersion, apk[0] ? apk : "a directory");

    create(&act, NULL, 0);
    fprintf(stderr, "[nativeact] ANativeActivity_onCreate returned\n");

    /* the lifecycle Android drives, in its order */
    if (cbs.onStart) cbs.onStart(&act);
    if (cbs.onResume) cbs.onResume(&act);
    if (cbs.onNativeWindowCreated) cbs.onNativeWindowCreated(&act, win);
    if (cbs.onNativeWindowResized) cbs.onNativeWindowResized(&act, win);
    if (cbs.onNativeWindowRedrawNeeded) cbs.onNativeWindowRedrawNeeded(&act, win);
    if (cbs.onContentRectChanged) {
        ARect rect = { 0, 0, g_window_w, g_window_h };
        cbs.onContentRectChanged(&act, &rect);
    }
    g_queue = android_input_queue_create();
    if (g_queue && cbs.onInputQueueCreated) cbs.onInputQueueCreated(&act, g_queue);
    if (cbs.onWindowFocusChanged) cbs.onWindowFocusChanged(&act, 1);
    egl_glue_set_swap_hook(na_swap_hook, NULL);
    fprintf(stderr, "[nativeact] running\n");

    /* the engine may have asked for the OBB fetch during create/resume */
    na_fire_obb_complete(act.env, act.clazz);

    while (!g_finish && !display_quit_requested()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) on_sdl_event(&ev);
        na_pointer_tick();
        na_fire_obb_complete(act.env, act.clazz);   /* also if OBBFetch came later (once only) */
        usleep(4000);
    }

    fprintf(stderr, "[nativeact] stopping\n");
    if (cbs.onWindowFocusChanged) cbs.onWindowFocusChanged(&act, 0);
    if (cbs.onPause) cbs.onPause(&act);
    if (cbs.onStop) cbs.onStop(&act);
    if (g_queue && cbs.onInputQueueDestroyed) cbs.onInputQueueDestroyed(&act, g_queue);
    if (cbs.onNativeWindowDestroyed) cbs.onNativeWindowDestroyed(&act, win);
    if (cbs.onDestroy) cbs.onDestroy(&act);
    fflush(NULL);
    return 0;
}
