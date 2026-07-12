#pragma once
#ifndef CATA_SRC_MULTIPLAYER_PLAYER_SAVE_H
#define CATA_SRC_MULTIPLAYER_PLAYER_SAVE_H

#include <string>

#include "memory_fast.h"
#include "multiplayer_player_runtime.h"

struct multiplayer_player_load_result {
    shared_ptr_fast<multiplayer_player_runtime> runtime;
    std::string error;

    explicit operator bool() const {
        return runtime != nullptr;
    }
};

/** Serialize the Phase 0 player-scoped server snapshot schema on the simulation thread. */
std::string serialize_multiplayer_player_runtime( const multiplayer_player_runtime &runtime );

/**
 * Parse a player snapshot into an importing runtime.
 *
 * The registry starts a new session only after the whole snapshot validates.
 * Parsing avatar state is simulation-thread-only because it touches game registries and IDs.
 */
multiplayer_player_load_result deserialize_multiplayer_player_runtime(
    const std::string &serialized,
    const multiplayer_player_runtime::achievement_callback &achievement_attained,
    const multiplayer_player_runtime::achievement_callback &achievement_failed );

#endif // CATA_SRC_MULTIPLAYER_PLAYER_SAVE_H
