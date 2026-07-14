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

/** Shrinks the outer visible radius, if necessary, until a full scene fits the mobile budget. */
bool multiplayer_fit_scene_snapshot_to_payload_budget( multiplayer_scene_snapshot &snapshot,
        multiplayer_transport_payload &payload, std::string &error );

#endif // CATA_SRC_MULTIPLAYER_SCENE_H
