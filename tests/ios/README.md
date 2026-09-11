# iOS example app

A small SwiftUI app that builds the Jitter RNG into an iOS app the way an app
would embed it: `CMakeLists.txt` includes the library's own `CMakeLists.txt`
and links the static library into the app, and `jitterentropy-bridging.h`
makes its C API visible to Swift. The app allocates one collector at start-up;
one button shows its `jent_status()` document, the other 32 bytes of its output
in hex. Both are also written to the system log. Three more buttons replace the
collector with a new one - without flags, in FIPS (`JENT_FORCE_FIPS`) or in
NTG.1 (`JENT_NTG1`) mode - after running the power-on tests with the same
flags. Both modes require the collector's state to be locked and fail with
EMEM where it cannot be; the memory access region is never locked.

The *Timer thread* toggle above those buttons allocates the next collector with
`JENT_FORCE_INTERNAL_TIMER`: its time stamps then come from the library's timer
thread, a thread counting in a loop, rather than from the platform clock. The
status document says which one a collector uses (`internalTimer`). The library
makes that choice once for the process: after a collector passed its power-on
tests on the timer thread, every later one uses it too, toggle or not, and
NTG.1 - which forbids the timer thread, and is unavailable while the toggle is
on - fails with ENOTIME until the app is restarted.

- `Collector.swift` - one collector and the flags it is allocated with; every
  call into the library runs on its serial queue
- `JitterEntropyExampleApp.swift` - the UI

## Building

Xcode and CMake on macOS. `xcodebuild`, `simctl` and `devicectl` come from
the active developer directory, which has to be Xcode's rather than the
Command Line Tools': `sudo xcode-select -s /Applications/Xcode.app`, or
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` in the environment.
The Xcode generator writes an `.xcodeproj` that can also be opened in Xcode:

```
cmake -S tests/ios -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
cmake --build build-ios --config Debug -- -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
```

To run it in a booted simulator:

```
xcrun simctl install booted build-ios/Debug-iphonesimulator/JitterEntropyExample.app
xcrun simctl launch --console booted de.chronox.jitterentropy.example
```

## Running on a device

A device build is signed, so it needs a development team: an Apple ID added
in Xcode's Settings > Accounts, where a free personal team will do. Once Xcode
has loaded the account, `defaults read com.apple.dt.Xcode
IDEProvisioningTeamByIdentifier` lists its team IDs. The team is passed at
configure time, as the generator would overwrite a team picked in the
generated project, and `-allowProvisioningUpdates` lets `xcodebuild` create
the signing certificate and the provisioning profile. An App ID belongs to the
first team that registers it, so another team needs its own bundle identifier
in place of the default `de.chronox.jitterentropy.example`, set with
`-DBUNDLE_ID`:

```
cmake -S tests/ios -B build-ios-device -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<team id> -DBUNDLE_ID=<bundle id>
cmake --build build-ios-device --config Debug -- -sdk iphoneos \
    -allowProvisioningUpdates -allowProvisioningDeviceRegistration
```

The first build of a new profile can stop with "unable to read input file
... .mobileprovision"; a second run picks the profile up.

The phone needs Developer Mode, under Settings > Privacy & Security, which
takes a restart. `xcrun devicectl list devices` shows it with its name and
identifier, either of which `--device` takes:

```
xcrun devicectl device install app --device <device> \
    build-ios-device/Debug-iphoneos/JitterEntropyExample.app
xcrun devicectl device process launch --console --terminate-existing \
    --device <device> <bundle id>
```

`--console` shows the app's log, whether the collector was allocated and the
output of each button, until the app exits or `devicectl` is stopped. An app
signed by a personal team only launches once its developer is trusted on the
phone, under Settings > General > VPN & Device Management; until then the
launch fails with "profile has not been explicitly trusted by the user".
