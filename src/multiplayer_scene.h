#pragma once
#ifndef CATA_SRC_MULTIPLAYER_SCENE_H
#define CATA_SRC_MULTIPLAYER_SCENE_H

#include <cstdint>
#include <string>

#include "multiplayer_protocol.h"

class game;

bool multiplayer_build_visible_scene( game &simulation, const std::string &player_id,
                                      const std::string &character_id,
                                      std::uint64_t server_revision, int radius,
                                      multiplayer_scene_snapshot &snapshot,
                                      std::string &error );

#endif // CATA_SRC_MULTIPLAYER_SCENE_H
