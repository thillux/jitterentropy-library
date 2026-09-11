# iOS example app

A small SwiftUI app that builds the Jitter RNG into an iOS app the way an app
would embed it: `CMakeLists.txt` includes the library's own `CMakeLists.txt`
and links the static library into the app, and `jitterentropy-bridging.h`
makes its C API visible to Swift. The app allocates one collector at start-up;
one button shows its `jent_status()` document, the other 32 bytes of its output
in hex. Both are also written to the system log.

- `Collector.swift` - one collector; every call into the library runs on its
  serial queue
- `JitterEntropyExampleApp.swift` - the UI

## Building

Xcode and CMake on macOS. The Xcode generator writes an `.xcodeproj` that can
also be opened in Xcode:

```
cmake -S tests/ios -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
cmake --build build-ios --config Debug -- -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
```

To run it in a booted simulator:

```
xcrun simctl install booted build-ios/Debug-iphonesimulator/JitterEntropyExample.app
xcrun simctl launch --console booted de.chronox.jitterentropy.example
```

A build for a device needs a signing team, set in Xcode or with
`-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<team id>`.
