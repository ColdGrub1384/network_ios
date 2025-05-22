#!/bin/bash

if [ -z "$IOS_SYSTEM_XCFRAMEWORK_PATH" ]; then
    echo "Set IOS_SYSTEM_XCFRAMEWORK_PATH environment variable to compile network_ios"
    exit 1
fi

cp -r "$IOS_SYSTEM_XCFRAMEWORK_PATH" "ios_system.xcframework"

BUILD() {
    if [[ "$2" == "maccatalyst" ]]; then
        sdk="macosx"
        ADDITIONAL_FLAGS="-destination 'platform=macOS,variant=Mac Catalyst,arch=$3' SUPPORTS_MAC_CATALYST=YES"
    else
        sdk="$2"
        ADDITIONAL_FLAGS="-arch $3"
    fi
    eval xcodebuild -project "network_ios.xcodeproj" -scheme $1 -sdk $sdk -configuration Release SYMROOT="build_$2.$3" "$ADDITIONAL_FLAGS"
}

MAKE_FAT_FRAMEWORK() {
    build_dir="build_$2"
    mkdir -p "$build_dir"
    
    if [[ "$2" == "maccatalyst" ]]; then
        binary_path="$build_dir/$1.framework/Versions/Current/$1"
    else
        binary_path="$build_dir/$1.framework/$1"
    fi
    
    binaries=""
    for arch in $3; do
        cp -r "$build_dir.$arch/$1.framework" "$build_dir/"
        binaries="$frameworks $build_dir.$arch/$1.framework/$1"
    done
    
    rm "$binary_path"
    
    for arch in $3; do
        break
    done
    
    eval lipo -create $binaries -output "$binary_path"
}

## network_ios ##

BUILD network_ios iphoneos         arm64
BUILD network_ios iphonesimulator  arm64
BUILD network_ios iphonesimulator  x86_64

BUILD network_ios maccatalyst      arm64
BUILD network_ios maccatalyst      x86_64

BUILD network_ios watchos          armv7k
BUILD network_ios watchos          arm64_32
BUILD network_ios watchos          arm64
BUILD network_ios watchsimulator   arm64
BUILD network_ios watchsimulator   x86_64

BUILD network_ios appletvos        arm64
BUILD network_ios appletvsimulator arm64
BUILD network_ios appletvsimulator x86_64

for build_folder in build_*/Release-*; do
    containing_folder="$(dirname $build_folder)"
    mv $build_folder/* "$containing_folder/"
    rm -rf "$build_folder"
done

## Cleanup ##

rm -rf "ios_system.xcframework"

## Make Fat Frameworks ##

MAKE_FAT_FRAMEWORK "network_ios" "maccatalyst"      "arm64 x86_64"
MAKE_FAT_FRAMEWORK "network_ios" "iphonesimulator"  "arm64 x86_64"
MAKE_FAT_FRAMEWORK "network_ios" "watchos"          "armv7k arm64_32 arm64"
MAKE_FAT_FRAMEWORK "network_ios" "watchsimulator"   "arm64 x86_64"
MAKE_FAT_FRAMEWORK "network_ios" "appletvsimulator" "arm64 x86_64"

## Make XFrameworks ##

xcodebuild -create-xcframework \
    -framework "build_iphoneos.arm64/network_ios.framework" \
    -framework "build_maccatalyst/network_ios.framework" \
    -framework "build_watchos/network_ios.framework" \
    -framework "build_watchsimulator/network_ios.framework" \
    -framework "build_appletvos.arm64/network_ios.framework" \
    -framework "build_appletvsimulator/network_ios.framework" \
    -output    "network_ios.xcframework"
