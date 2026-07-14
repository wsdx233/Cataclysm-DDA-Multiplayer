@echo off
setlocal EnableExtensions DisableDelayedExpansion

for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
set "VERSION_HEADER=%REPO_ROOT%\src\version.h"

rem Preserve Visual Studio's bundled-Git fallback.
if not defined VSAPPIDDIR goto git_path_ready
for %%I in ("%VSAPPIDDIR%\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd") do set "VS_GIT=%%~fI"
if exist "%VS_GIT%\git.exe" set "PATH=%PATH%;%VS_GIT%"

:git_path_ready
set "VERSION="
for /f "usebackq delims=" %%I in (`git -C "%REPO_ROOT%" describe --tags --always --dirty --match "cdda-experimental-*" 2^>nul`) do set "VERSION=%%I"
if not defined VERSION set "VERSION=Please install `git` to generate VERSION"

setlocal EnableDelayedExpansion
set "BUILD_ID="
set "BUILD_ID_SOURCE=git HEAD"

if defined MULTIPLAYER_BUILD_ID (
    set "BUILD_ID=!MULTIPLAYER_BUILD_ID!"
    set "BUILD_ID_SOURCE=MULTIPLAYER_BUILD_ID override"
    goto build_id_ready
)

set "HEAD_CAPTURE=!VERSION_HEADER!.head.!RANDOM!.!RANDOM!.tmp"
git -C "!REPO_ROOT!" rev-parse --verify HEAD >"!HEAD_CAPTURE!" 2>nul
set "HEAD_EXIT=!errorlevel!"
if !HEAD_EXIT! EQU 0 set /p "BUILD_ID="<"!HEAD_CAPTURE!"
del /q "!HEAD_CAPTURE!" >nul 2>&1

rem No usable Git HEAD and no override deliberately leaves the ID empty.
if not !HEAD_EXIT! EQU 0 goto build_id_ready
if not defined BUILD_ID goto invalid_build_id

git -C "!REPO_ROOT!" diff --quiet HEAD
set "DIFF_EXIT=!errorlevel!"
if !DIFF_EXIT! EQU 1 set "BUILD_ID=!BUILD_ID!-dirty"
if !DIFF_EXIT! LEQ 1 goto build_id_ready

>&2 echo Unable to determine dirty state with git diff --quiet HEAD ^(exit !DIFF_EXIT!^)
exit /b !DIFF_EXIT!

:build_id_ready
if not defined BUILD_ID goto write_header
call :validate_build_id
if not errorlevel 1 goto write_header

:invalid_build_id
>&2 echo Invalid !BUILD_ID_SOURCE!: expected 40 lowercase hexadecimal characters with optional -dirty
exit /b 1

:write_header
set "TEMP_HEADER=!VERSION_HEADER!.tmp.!RANDOM!.!RANDOM!"
(
    echo // NOLINT^(cata-header-guard^)
    echo #define VERSION "!VERSION!"
    echo #define MULTIPLAYER_BUILD_ID "!BUILD_ID!"
) >"!TEMP_HEADER!" || goto header_write_failed

if not exist "!VERSION_HEADER!" goto replace_header

fc /b "!TEMP_HEADER!" "!VERSION_HEADER!" >nul 2>&1
set "COMPARE_EXIT=!errorlevel!"
if !COMPARE_EXIT! EQU 0 goto header_unchanged
if !COMPARE_EXIT! EQU 1 goto replace_header

>&2 echo Unable to compare generated version header with "!VERSION_HEADER!"
del /q "!TEMP_HEADER!" >nul 2>&1
exit /b 1

:header_unchanged
del /q "!TEMP_HEADER!" >nul 2>&1
exit /b 0

:replace_header
move /y "!TEMP_HEADER!" "!VERSION_HEADER!" >nul
if errorlevel 1 goto header_move_failed
echo Generated "version.h".
echo VERSION defined as "!VERSION!"
echo MULTIPLAYER_BUILD_ID defined as "!BUILD_ID!"
exit /b 0

:header_write_failed
>&2 echo Unable to write temporary version header.
del /q "!TEMP_HEADER!" >nul 2>&1
exit /b 1

:header_move_failed
>&2 echo Unable to replace "!VERSION_HEADER!".
del /q "!TEMP_HEADER!" >nul 2>&1
exit /b 1

:validate_build_id
rem Accept exactly 40 characters or 40 plus the six-character "-dirty".
set "CHECK_CHAR=!BUILD_ID:~39,1!"
if not defined CHECK_CHAR exit /b 1
set "CHECK_SUFFIX=!BUILD_ID:~40!"
if defined CHECK_SUFFIX if not "!CHECK_SUFFIX!"=="-dirty" exit /b 1

:validate_build_id_base
set "CHECK_REMAINDER=!BUILD_ID:~0,40!"
for %%C in (0 1 2 3 4 5 6 7 8 9 a b c d e f) do set "CHECK_REMAINDER=!CHECK_REMAINDER:%%C=!"
if defined CHECK_REMAINDER exit /b 1
exit /b 0
