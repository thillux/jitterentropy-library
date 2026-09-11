# Android example app

A small app that builds the Jitter RNG into an Android app the way an app
would embed it: `app/src/main/cpp/CMakeLists.txt` includes the library's own
`CMakeLists.txt` and links it into a JNI library, `libjitterentropy_jni.so`.
The app allocates one collector at start-up; one button shows its
`jent_status()` document, the other 32 bytes of its output in hex. Both are
also written to logcat under the tag `JitterEntropyExample`.

- `app/src/main/cpp/jitterentropy-jni.c` - the JNI binding
- `app/src/main/java/.../JitterEntropy.kt` - the Kotlin side of it, one
  collector per instance
- `app/src/main/java/.../CollectorViewModel.kt` - owns the collector across
  activity recreation; every call into the library runs on one background
  thread
- `app/src/main/java/.../MainActivity.kt` - the UI, in Jetpack Compose with
  Material 3

## Building with Nix

```
nix build .#android-example            # result/jitterentropy-example.apk
nix run .#android-example-emulator     # boot an emulator, install, start
```

The emulator needs `/dev/kvm`. Set `NIX_ANDROID_EMULATOR_FLAGS=-no-window` to
run it without a display, then follow the app with
`adb logcat -s JitterEntropyExample`.

The APK is signed with a debug key generated afresh by every build, so a newer
build does not install over an older one: `adb uninstall
de.chronox.jitterentropy.example` first.

The Nix build runs offline, so Gradle cannot fetch the Android Gradle plugin,
the Compose compiler or the AndroidX libraries itself. `deps.json` locks every
file it needs, and Nix serves them to it. After changing a plugin or library
version, or anything else Gradle downloads, regenerate the lock from the
repository root:

```
$(nix build --no-link --print-out-paths .#android-example.mitmCache.updateScript)
```

The SDK components the build uses - platform, build tools, CMake and NDK - are
pinned in `app/build.gradle.kts` and composed to match in `flake.nix`; change
them in both places.

## Building without Nix

Open this directory in Android Studio, or run `gradle assembleDebug` with a
Gradle the Android Gradle plugin in `build.gradle.kts` supports (the Nix build
uses 9.5) and `ANDROID_HOME` pointing at an SDK. No Gradle wrapper is checked
in.

## ndk-build

`Android.mk` builds the library alone, as `libjitterentropy.so`, for projects
built with ndk-build (`nix build .#android` uses it):

```
ndk-build NDK_PROJECT_PATH=null APP_BUILD_SCRIPT=$PWD/tests/android/Android.mk \
          APP_PLATFORM=android-21
```
