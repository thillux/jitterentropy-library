plugins {
    id("com.android.application")
}

android {
    namespace = "de.chronox.jitterentropy.example"
    compileSdk = 36
    // Pinned rather than left to AGP's default so that the SDK flake.nix
    // composes is the one the build finds; neither may download anything.
    buildToolsVersion = "36.1.0"
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "de.chronox.jitterentropy.example"
        // The NDK's floor, as for the ndk-build target in flake.nix: below
        // API 28 the library takes its /dev/urandom fallback for getrandom().
        minSdk = 21
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
