cmake_minimum_required(VERSION 3.22)

# Configure with the Xcode generator for iOS, see README.md:
#   cmake -S tests/ios -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
project(jitterentropy_example LANGUAGES C Swift)

# The probes of the library's CMakeLists.txt would otherwise link an iOS
# executable, which Xcode wants signed.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# Swift 5 mode: the collector is handed between the main queue and its own
# serial queue, which the Swift 6 strict concurrency checks reject without
# annotations that would only obscure an example.
set(CMAKE_Swift_LANGUAGE_VERSION 5.0)

# The library is built from this tree through its own CMakeLists.txt, the way
# an app vendoring it would. Only the library: the tools and the test suite are
# host programs.
set(JENT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${JENT_ROOT} jitterentropy EXCLUDE_FROM_ALL)

add_executable(JitterEntropyExample MACOSX_BUNDLE
    JitterEntropyExampleApp.swift
    Collector.swift)
set_target_properties(JitterEntropyExample PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER de.chronox.jitterentropy.example
    MACOSX_BUNDLE_BUNDLE_NAME JitterEntropyExample
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER de.chronox.jitterentropy.example
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 15.0
    # iPhone and iPad
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    # The C API reaches Swift through this header; the library target
    # supplies the include paths it needs.
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-bridging.h)
target_link_libraries(JitterEntropyExample PRIVATE jitterentropy)
