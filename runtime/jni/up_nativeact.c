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
 */
#define _GNU_SOURCE
#include "up_internal.h"

#include <stdlib.h>
#include <string.h>

/* the class the engine's activity object has (found in libBS1R.so); the JNI natives it exports
 * live in uk/co/cyborg/napp/MainActivity, which is this class' package on the device */
#define NA_MAIN   "uk/co/revolution/bs1r/google/MainActivity"
#define NA_NATIVE "uk/co/cyborg/napp/MainActivity"

static jvalue na_str(const char *s) { return fj_l(fj_string(s ? s : "")); }

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
    fj_class_define(NA_MAIN, "android/app/Activity");
    fj_class_define(NA_NATIVE, NA_MAIN);

    reg("getLocaleLanguage", "()Ljava/lang/String;", na_language);
    reg("getLocaleCountry", "()Ljava/lang/String;", na_country);
    /* the signature seen in the .so beside the engine's AndroidJNI::TextRenderBitmap */
    reg("textRenderBitmap", "(ILjava/lang/String;ILjava/lang/String;II)Landroid/graphics/Bitmap;", na_null);
    reg("textRenderBitmap", "(Ljava/lang/String;IILjava/lang/String;II)Landroid/graphics/Bitmap;", na_null);

    reg("platformIsSignedIn", "()Z", na_false);
    reg("platformIsSigningIn", "()Z", na_false);
    reg("platformSaveGamesAvailable", "()Z", na_true);
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
}

/* The ANativeActivity's clazz: an instance of the engine's activity class, kept for the process. */
jobject up_nativeact_activity(void)
{
    static jobject act;
    up_nativeact_register();
    if (!act) act = fj_global(fj_new_by_name(NA_MAIN, NULL, NULL));
    return act;
}
