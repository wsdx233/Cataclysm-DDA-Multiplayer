#pragma once
#ifndef CATA_SRC_GET_VERSION_H
#define CATA_SRC_GET_VERSION_H
const char *getVersionString();

// Canonical source identity compiled into every multiplayer artifact.  Unlike the
// display version, this excludes UI backend suffixes and generator-specific tags.
const char *getMultiplayerBuildId();
#endif // CATA_SRC_GET_VERSION_H
