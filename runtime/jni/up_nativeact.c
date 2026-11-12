/*
 * up_nativeact.c - the fake Java side of a game's own NativeActivity (Broken Sword Reforged's
 * uk/co/revolution/bs1r/google/MainActivity, a custom GLES2 engine).
 *
 * Unlike Unity's UnityPlayer there is no Java framework here: the engine opens its Android
 * context, then calls a few platform helpers on its activity. The native methods it exports
 * (Java_..._MainActivity_platform*) are never called (no Java code exists); the engine -> Java
 * calls it makes are answered here. Names not registered stay fakejni placeholders (0/NULL),
 * which the engine has to tolerate: FJ_DUMP=1 lists them.
 *
 * What the engine asks for and what it gets:
 *   getLocaleLanguage / getLocaleCountry   NA_LANGUAGE / NA_COUNTRY ("en"/"US")
 *   platformIsSignedIn / IsSigningIn       false: no Play Games account (cloud saves and
 *   platformSaveGamesAvailable             achievements are skipped; local saves work)
 *   platform* (sign-in overlay, review, achievements, save-game read/write, OBB/asset-pack
 *   fetch)                                 no-ops; the completions stay pending, which is what a
 *                                          phone without the Play services does
 *   textRenderBitmap                       NULL (the engine's FreeType path draws text)
 *   getScreenSize / getAvailableMemory     what libBS1R's JNI_OnLoad registered them as natives
 *                                          on NA_NATIVE; if they are not registered (or answer 0)
 *                                          the host window's height and the machine's free memory.
 *                                          The fake framework has no DisplayMetrics/MemoryInfo for
 *                                          a native to read, so 0 would size the display/FBOs 0.
 *   OBBFetch                               marks the OBB fetch pending; the host fires the
 *                                          engine's Java_..._MainActivity_OBBFetchComplete after
 *                                          the activity exists (the assets come from the APK, no
 *                                          OBB is fetched)
 */
#define _GNU_SOURCE
#include "up_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The APK's activity classes: uk/co/cyborg/napp/MainActivity is the NativeActivity that declares
 * (and libBS1R.so registers in JNI_OnLoad) the platform natives; the engine's activity object is
 * its subclass uk/co/revolution/bs1r/google/MainActivity, the manifest's activity, so the natives
 * are inherited and found on it. */
#define NA_MAIN   "uk/co/revolution/bs1r/google/MainActivity"
#define NA_NATIVE "uk/co/cyborg/napp/MainActivity"

/* The ANativeWindow shims (runtime/android) are in the host binary, not in libfakejni.a (the JNI
 * unit tests link the library without them): use the window only where the host has it. */
typedef struct ANativeWindow ANativeWindow;
extern ANativeWindow *android_window_get(void) __attribute__((weak));
extern int32_t ANativeWindow_getHeight(ANativeWindow *w) __attribute__((weak));

static jvalue na_str(const char *s) { return fj_l(fj_string(s ? s : "")); }

/* The host window's height in pixels (480 on the test device). */
static int na_screen_h(void)
{
    if (android_window_get && ANativeWindow_getHeight) {
        ANativeWindow *w = android_window_get();
        if (w) {
            int h = ANativeWindow_getHeight(w);
            if (h > 0) return h;
        }
    }
    return 480;
}

/* MemAvailable of /proc/meminfo in bytes; the engine only needs a plausible number. */
static jint na_memory_available(void)
{
    long kb = 0;
    char line[128];
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
        fclose(f);
    }
    if (kb < 64 * 1024) kb = 512 * 1024;      /* unreadable: half a gigabyte */
    long long b = (long long)kb * 1024;
    return (jint)(b > 0x7fffffff ? 0x7fffffff : b);
}

/* The class of the activity chain that libBS1R registered `name` on, or NULL. */
static const char *na_native_class(const char *name, const char *sig)
{
    if (fj_native(NA_NATIVE, name, sig)) return NA_NATIVE;
    if (fj_native(NA_MAIN, name, sig)) return NA_MAIN;
    return NULL;
}

H(na_language)
{
    (void)self; (void)a;
    const char *v = getenv("NA_LANGUAGE");
    return na_str(v && *v ? v : "en");
}

H(na_country)
{
    (void)self; (void)a;
    const char *v = getenv("NA_COUNTRY");
    return na_str(v && *v ? v : "US");
}

H(na_true) { (void)self; (void)a; return fj_z(JNI_TRUE); }
H(na_false) { (void)self; (void)a; return fj_z(JNI_FALSE); }
H(na_null) { (void)self; (void)a; return fj_l(NULL); }
H(na_void) { (void)self; (void)a; return fj_v(); }

/* OBBFetch is Java-side dex code in the APK: the engine calls it and then waits for the exported
 * Java_uk_co_cyborg_napp_MainActivity_OBBFetchComplete to be called. The assets are in the APK and
 * no OBB exists, so the completion is reported at once — but not from here: this runs inside the
 * engine's own Java call, and re-entering the game would nest its JNI use. Only mark it pending;
 * the host (runtime/host/nativeact.c) fires the callback on the activity thread. */
static volatile int g_obb_pending;

int up_nativeact_obb_pending(void) { return g_obb_pending; }

H(na_obb_fetch)
{
    (void)self; (void)a;
    fprintf(stderr, "[na] OBBFetch\n");
    g_obb_pending = 1;
    return fj_v();
}

H(na_screen_size)
{
    (void)a;
    const char *cls = na_native_class("getScreenSize", "()F");
    if (cls) {
        jvalue r = fj_call_native(cls, "getScreenSize", "()F", self);
        if (r.f > 0.0f) return r;
    }
    return fj_f((jfloat)na_screen_h());
}

H(na_memory)
{
    (void)a;
    const char *cls = na_native_class("getAvailableMemory", "()I");
    if (cls) {
        jvalue r = fj_call_native(cls, "getAvailableMemory", "()I", self);
        if (r.i > 0) return r;
    }
    return fj_i(na_memory_available());
}

/* Every method the engine's Java side could call, as an instance method and as a static one
 * (which of the two Call*Method the engine uses is not visible in its imports). */
static void reg(const char *name, const char *sig, fj_fn fn)
{
    fj_method_define(NA_MAIN, name, sig, 0, fn, NULL);
    fj_method_define(NA_MAIN, name, sig, 1, fn, NULL);
}

void up_nativeact_register(void)
{
    static int done;
    if (done) return;
    done = 1;
    fj_init();
    fj_class_define("android/app/Activity", NULL);
    fj_class_define("android/app/NativeActivity", "android/app/Activity");
    fj_class_define(NA_NATIVE, "android/app/NativeActivity");
    fj_class_define(NA_MAIN, NA_NATIVE);

    reg("getLocaleLanguage", "()Ljava/lang/String;", na_language);
    reg("getLocaleCountry", "()Ljava/lang/String;", na_country);
    /* the signature seen in the .so beside the engine's AndroidJNI::TextRenderBitmap */
    reg("textRenderBitmap", "(ILjava/lang/String;ILjava/lang/String;II)Landroid/graphics/Bitmap;", na_null);
    reg("textRenderBitmap", "(Ljava/lang/String;IILjava/lang/String;II)Landroid/graphics/Bitmap;", na_null);

    reg("platformIsSignedIn", "()Z", na_false);
    reg("platformIsSigningIn", "()Z", na_false);
    reg("platformSaveGamesAvailable", "()Z", na_false); /* no cloud saves: engine hangs waiting for platformSaveGameRead completion */
    reg("platformSignIn", "()V", na_void);
    reg("platformSignOut", "()V", na_void);
    reg("platformShowSignInOverlay", "()V", na_void);
    reg("platformHideSignInOverlay", "()V", na_void);
    reg("platformShowAchievements", "()V", na_void);
    reg("platformReviewGame", "()V", na_void);
    reg("platformUnlockAchievement", "(Ljava/lang/String;)V", na_void);
    reg("platformSaveGameRead", "(Ljava/lang/String;)V", na_void);
    reg("platformSaveGameWrite", "(Ljava/lang/String;[B)V", na_void);
    reg("assetPackFetch", "(Ljava/lang/String;)V", na_void);

    /* The natives libBS1R registers on NA_NATIVE in JNI_OnLoad (which runs before this, so the
     * registration is recorded on the same class object): the handler delegates to the registered
     * native, and answers with the host values if it is absent or answers 0 (see the header). */
    fj_method_define(NA_NATIVE, "getScreenSize", "()F", 0, na_screen_size, NULL);
    fj_method_define(NA_NATIVE, "getScreenSize", "()F", 1, na_screen_size, NULL);
    fj_method_define(NA_NATIVE, "getAvailableMemory", "()I", 0, na_memory, NULL);
    fj_method_define(NA_NATIVE, "getAvailableMemory", "()I", 1, na_memory, NULL);
    fj_method_define(NA_NATIVE, "OBBFetch", "()V", 0, na_obb_fetch, NULL);
    fj_method_define(NA_NATIVE, "OBBFetch", "()V", 1, na_obb_fetch, NULL);
}

/* The ANativeActivity's clazz: an instance of the engine's activity class, kept for the process. */
jobject up_nativeact_activity(void)
{
    static jobject act;
    up_nativeact_register();
    if (!act) act = fj_global(fj_new_by_name(NA_MAIN, NULL, NULL));
    return act;
}
