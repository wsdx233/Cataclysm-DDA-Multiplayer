#!/usr/bin/env bash

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    printf 'Source this file instead: source %s\n' "$0" >&2
    exit 1
fi

export JAVA_HOME="${JAVA_HOME:-$HOME/.local/opt/temurin-17}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
export CDDA_LINUX_TOOLCHAIN_ROOT="${CDDA_LINUX_TOOLCHAIN_ROOT:-$HOME/.local/toolchains/cdda-linux}"

path_entries=(
    "$CDDA_LINUX_TOOLCHAIN_ROOT/usr/bin"
    "$JAVA_HOME/bin"
    "$ANDROID_HOME/platform-tools"
    "$ANDROID_HOME/cmdline-tools/latest/bin"
    "$ANDROID_HOME/build-tools/34.0.0"
    "$ANDROID_HOME/cmake/3.22.1/bin"
    "$HOME/.local/bin"
)

for path_entry in "${path_entries[@]}"; do
    case ":$PATH:" in
        *":$path_entry:"*) ;;
        *) PATH="$path_entry:$PATH" ;;
    esac
done

export PATH

library_entries=(
    "$CDDA_LINUX_TOOLCHAIN_ROOT/usr/lib/x86_64-linux-gnu"
    "$CDDA_LINUX_TOOLCHAIN_ROOT/usr/lib/gcc/x86_64-linux-gnu/13"
)

for library_entry in "${library_entries[@]}"; do
    case ":${LIBRARY_PATH:-}:" in
        *":$library_entry:"*) ;;
        *) LIBRARY_PATH="$library_entry${LIBRARY_PATH:+:$LIBRARY_PATH}" ;;
    esac
done

export LIBRARY_PATH

for library_entry in "${library_entries[@]}"; do
    case ":${LD_LIBRARY_PATH:-}:" in
        *":$library_entry:"*) ;;
        *) LD_LIBRARY_PATH="$library_entry${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
    esac
done

export LD_LIBRARY_PATH

include_entries=(
    "$CDDA_LINUX_TOOLCHAIN_ROOT/usr/include"
    "$CDDA_LINUX_TOOLCHAIN_ROOT/usr/include/ncursesw"
)

for include_entry in "${include_entries[@]}"; do
    case ":${CPATH:-}:" in
        *":$include_entry:"*) ;;
        *) CPATH="$include_entry${CPATH:+:$CPATH}" ;;
    esac
done

export CPATH
