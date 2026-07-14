#include "get_version.h" // IWYU pragma: associated

#if (defined(_WIN32) || defined(MINGW)) && !defined(GIT_VERSION) && !defined(CROSS_LINUX) && !defined(_MSC_VER)

#ifndef VERSION
#define VERSION "0.J"
#endif

#else

#include "version.h"

#endif

#ifndef MULTIPLAYER_BUILD_ID
#define MULTIPLAYER_BUILD_ID ""
#endif

const char *getVersionString()
{
#if defined(USE_SDL3)
    return VERSION "+SDL3";
#else
    return VERSION;
#endif
}

const char *getMultiplayerBuildId()
{
    return MULTIPLAYER_BUILD_ID;
}
