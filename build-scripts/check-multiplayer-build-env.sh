#!/usr/bin/env bash

set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:-all}"
failures=0

ok()
{
    printf '[ok] %s\n' "$1"
}

warn()
{
    printf '[warn] %s\n' "$1"
}

fail()
{
    printf '[missing] %s\n' "$1"
    failures=$(( failures + 1 ))
}

require_command()
{
    local command_name="$1"
    if command -v "$command_name" >/dev/null 2>&1; then
        ok "$command_name: $(command -v "$command_name")"
    else
        fail "$command_name"
    fi
}

check_linux()
{
    local compiler=""

    printf '\nLinux curses baseline\n'
    require_command git
    require_command make
    require_command cmake
    require_command msgfmt

    if command -v clang++-18 >/dev/null 2>&1; then
        compiler="$(command -v clang++-18)"
    elif command -v g++-13 >/dev/null 2>&1; then
        compiler="$(command -v g++-13)"
    elif command -v g++ >/dev/null 2>&1; then
        compiler="$(command -v g++)"
    else
        fail 'clang++-18 or g++'
    fi

    if [ -n "$compiler" ]; then
        ok "compiler: $compiler"
        if printf '#include <zconf.h>\n#include <bzlib.h>\n' |
           "$compiler" -E -x c++ - >/dev/null 2>&1; then
            ok 'zlib and bzip2 development files'
        else
            fail 'zlib and bzip2 development files'
        fi
    fi

    if command -v ccache >/dev/null 2>&1; then
        ok "ccache: $(command -v ccache)"
    else
        warn 'ccache is optional for local builds; CI enables it'
    fi

    if command -v ncursesw6-config >/dev/null 2>&1 ||
       { command -v pkg-config >/dev/null 2>&1 && pkg-config --exists ncursesw; }; then
        ok 'ncursesw development files'
    else
        fail 'ncursesw development files'
    fi
}

check_android()
{
    printf '\nAndroid arm64 baseline\n'
    require_command java
    require_command unzip

    if command -v java >/dev/null 2>&1; then
        java_version="$(java -version 2>&1 | head -n 1)"
        if printf '%s' "$java_version" | grep -Eq 'version "17([.]|\")'; then
            ok "$java_version"
        else
            fail "JDK 17 required; found $java_version"
        fi
    fi

    android_root="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [ -z "$android_root" ]; then
        fail 'ANDROID_HOME or ANDROID_SDK_ROOT'
        return
    fi

    if [ -d "$android_root" ]; then
        ok "Android SDK: $android_root"
    else
        fail "Android SDK directory: $android_root"
        return
    fi

    if command -v sdkmanager >/dev/null 2>&1 || find "$android_root/cmdline-tools" -type f -name sdkmanager -print -quit 2>/dev/null | grep -q .; then
        ok 'sdkmanager'
    else
        fail 'sdkmanager'
    fi

    if [ -d "$android_root/platforms/android-35" ]; then
        ok 'Android platform 35'
    else
        fail 'Android platform 35'
    fi

    if [ -d "$android_root/build-tools/34.0.0" ]; then
        ok 'Android Build Tools 34.0.0'
    else
        fail 'Android Build Tools 34.0.0'
    fi

    if [ -d "$android_root/cmake/3.22.1" ]; then
        ok 'Android CMake 3.22.1'
    else
        fail 'Android CMake 3.22.1'
    fi

    if [ -d "$android_root/ndk/28.1.13356709" ]; then
        ok 'Android NDK 28.1.13356709'
    else
        fail 'Android NDK 28.1.13356709'
    fi

    if [ -x "$repo_root/android/gradlew" ]; then
        ok 'Android Gradle wrapper'
    else
        fail 'android/gradlew is not executable'
    fi
}

case "$mode" in
    linux)
        check_linux
        ;;
    android)
        check_android
        ;;
    all)
        check_linux
        check_android
        ;;
    *)
        printf 'Usage: %s [linux|android|all]\n' "$0" >&2
        exit 2
        ;;
esac

if [ "$failures" -ne 0 ]; then
    printf '\nEnvironment check failed with %d missing requirement(s).\n' "$failures" >&2
    exit 1
fi

printf '\nEnvironment check passed.\n'
