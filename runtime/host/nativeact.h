// NativeActivity host (ua_host UA_NATIVEACTIVITY=1): runs a game whose engine is its own
// ANativeActivity library (Broken Sword Reforged's libBS1R.so) on the same shims, display and
// input as ua_host's Unity path. The lifecycle, the fake ANativeActivity and the SDL -> Android
// input mapping live in nativeact.c; the fake Java side is runtime/jni/up_nativeact.c; assets
// come from the APK's stored `assets/` members (runtime/android/asset_manager.c,
// android_set_asset_apk).
//
// Environment:
//   UA_NATIVEACTIVITY=1   ua_host runs this host instead of a Unity player
//   NA_LIB=<path>         the engine library (default <root>/lib/libBS1R.so)
//   NA_APK=<path>         the APK holding `assets/` (default android_package.json's "apk", else
//                         the only .apk in <root>)
//   NA_ASSET_DIR=<dir>    assets from a directory instead of the APK (unpacked assets/)
//   NA_SDK_INT=<n>        Build.VERSION.SDK_INT reported (default 29)
//   NA_LANGUAGE, NA_COUNTRY  what the engine's Java getLocale* return ("en", "US")
//   NA_OBB=<path>         ANativeActivity.obbPath (default NULL: no OBB)
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 0 when the game ran and stopped normally, 1 on a setup error (logged).
// `root` is the game's directory (the working directory, as for ua_host's Unity path).
int nativeact_run(const char *root, const char *apk_name);

#ifdef __cplusplus
}
#endif
